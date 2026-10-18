/* box_view: carcasa de visión interior (VR 6DOF). C99, sin dependencias de plataforma; se prueba en headless.
 *
 * En el original, con el objeto "caja" equipado y en primera persona, la pantalla se cubre con una máscara 2D
 * opaca (negra) con una abertura alargada de esquinas redondeadas, algo por encima del centro, con un degradado
 * semitransparente en su borde interior que se anima abriéndose (8 pasos) al aparecer. Aquí se reproduce como
 * geometría 3D: una carcasa cerrada alrededor de la cabeza con un panel frontal con esa abertura (mismo contorno y
 * misma proporción angular con el FOV del juego), cantos con grosor, interior de cartón (textura del disco) o
 * negro puro (estilo fiel), bajada/subida sobre la cabeza y retardo opcional.
 *
 * Marco de la carcasa ("local"): origen = punto medio entre los ojos, +Y arriba, -Z hacia delante, +X derecha;
 * metros. La app dibuja box_view_draws() con la pose mundo indicada. El mundo del juego lo dibuja la app por su
 * cuenta (estéreo real); la carcasa solo lo tapa fuera de la abertura.
 *
 * Uso:
 *   BoxViewConfig c; box_view_default_config(&c); c.stage = "..."; c.sight_id = ...; c.tex_id = ...;   // la app los pasa
 *   box_view_init(&c, stage_dir, stage_dir_len);                  // 0 = datos del disco, 1 = respaldo genérico
 *   cada frame:  box_view_set_active(caja_equipada);              // lo decide el puente con la RAM del juego
 *                box_view_update(head_pose, dt);
 *                BoxDraw d[BOX_MAX_DRAWS]; n = box_view_draws(d, BOX_MAX_DRAWS);   // vacío si no es visible
 *   (revisión: si box_view_revision() cambia, re-subir los vértices de las mallas dinámicas)
 */
#pragma once
#include "../weapons/weapon.h"
#include <stddef.h>
#include <stdint.h>

#define BOX_MAX_DRAWS 3

typedef enum { BOX_STYLE_CARDBOARD = 0, BOX_STYLE_BLACK = 1 } BoxStyle;
typedef enum { BOX_BLEND_OPAQUE = 0, BOX_BLEND_ALPHA = 1 } BoxBlend;
typedef enum { BOX_OFF = 0, BOX_ENTERING, BOX_ACTIVE, BOX_LEAVING } BoxState;

typedef struct {
    /* datos del disco (los pasa la app; NULL = respaldo genérico sin textura) */
    const char *stage;          /* stage con los recursos */
    uint16_t sight_id, tex_id;  /* id16 de la máscara (tipo 's') y de la textura (tipo 'p') */
    int tex_y0, tex_y1;         /* filas de la textura usadas como baldosa lisa (sin impresos) */
    /* proyección del juego */
    float focal_px;             /* distancia focal en píxeles del lienzo original (320) */
    float view_cy;              /* fila del lienzo donde está el centro óptico (120) */
    /* carcasa (m) */
    float distance;             /* panel frontal, delante de los ojos (0.26) */
    float rim;                  /* grosor del canto de la abertura (0.012) */
    float half_w, top, bottom, back;   /* interior: lados, techo, suelo (bajo los ojos) y fondo (detrás de los ojos) */
    float ipd;                  /* separación entre ojos, solo para utilidades de medida (0.064) */
    BoxStyle style;             /* BOX_STYLE_CARDBOARD (defecto) o BOX_STYLE_BLACK (máscara negra del original) */
    /* inercia de la carcasa respecto a la cabeza (solo giro) */
    float lag_rate;             /* 1/s; 0 = sin retardo (la carcasa va pegada a la cabeza) */
    float lag_max_deg;          /* desfase angular máximo permitido (la abertura nunca sale de delante) */
    /* transición */
    float drop_time;            /* s: bajar/subir la caja */
    float open_time;            /* s: apertura de la abertura (8 pasos del original) */
    float lift;                 /* m: altura inicial sobre la cabeza antes de bajar */
    /* accesibilidad: abertura más ancha (desactivada por defecto) */
    float wide_x, wide_y;       /* factor de ampliación del contorno al activar box_view_set_wide */
} BoxViewConfig;

typedef struct {
    const WMesh *mesh;          /* vértices en el marco local de la carcasa */
    WPose pose;                 /* local -> mundo */
    BoxBlend blend;
    float alpha;                /* multiplicador de opacidad del dibujo (1 salvo en la transición) */
    int texture;                /* 0 = sin textura; 1 = la de box_view_texture() */
    int dynamic;                /* 1 = los vértices cambian con la apertura (ver box_view_revision) */
} BoxDraw;

typedef struct {                /* extensión angular de la abertura vista desde el punto medio entre los ojos */
    float left, right, up, down;   /* rad, positivos hacia fuera desde -Z */
    float center_pitch;            /* rad: elevación del centro de la abertura respecto a la mirada */
} BoxAperture;

void box_view_default_config(BoxViewConfig *c);
/* 0 = datos leídos del disco; 1 = respaldo genérico (sin datos o no encontrados); <0 = error de memoria. */
int  box_view_init(const BoxViewConfig *c, const uint8_t *dir, size_t dir_len);
void box_view_shutdown(void);

void box_view_set_active(int on);            /* lo llama el puente cuando el objeto está equipado y en uso */
int  box_view_is_active(void);               /* último valor pedido */
void box_view_update(WPose head, float dt);  /* pose de la cabeza (punto medio entre los ojos), mundo de la app */

int      box_view_visible(void);             /* 1 si hay algo que dibujar (no BOX_OFF) */
BoxState box_view_state(void);
float    box_view_drop(void);                /* 0 = levantada, 1 = puesta */
float    box_view_open(void);                /* 0 = abertura cerrada (inicial), 1 = abierta del todo */
int      box_view_draws(BoxDraw *out, int max);
const uint8_t *box_view_texture(int *w, int *h);   /* RGBA8 de la baldosa de cartón; NULL en estilo negro o sin datos */
uint32_t box_view_revision(void);            /* cambia cuando cambian los vértices dinámicos */
WPose    box_view_shell_pose(void);          /* local -> mundo de la carcasa (con bajada e inercia) */

void box_view_set_wide(int on);              /* accesibilidad: abertura más ancha (defecto: desactivada) */
int  box_view_is_wide(void);
void box_view_set_style(BoxStyle s);
void box_view_set_lag(float rate, float max_deg);

/* Extensión angular de la abertura (con apertura completa y ampliación actual). 0 = bien. */
int  box_view_aperture(BoxAperture *out);
/* Contorno actual de la abertura (con apertura completa y ampliación actual) en el marco local, sobre el panel
 * frontal, ordenado por ángulo. Devuelve el nº de puntos escritos (<= max). */
int  box_view_outline(WVec3 *out, int max);
/* Proyección de un punto del lienzo original (px) al marco local sobre el panel frontal (m). */
WVec3 box_view_px_to_local(float px, float py);
