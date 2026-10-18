/* box_data: lectura, en tiempo de ejecución, de los recursos de la "vista interior" (máscara de 2D del original)
 * y de la textura de cartón desde el archivo de stages del usuario. C99, sin dependencias de plataforma.
 * Los datos NO viajan con la app ni con el repo: se leen del disco del usuario. Los identificadores (stage, id16)
 * los pasa la app por configuración; aquí no hay nombres del juego.
 *
 * ---- Formato del recurso de máscara (tipo 's', id24 = (0x73 << 16) | id16) ----
 *   +0  u16 longitud de la región de primitivas
 *   +2  u8  flags
 *   +3  u8  nprims
 *   +4  u32 off_entradas   (u32 off_prims   u32 off_idx   u32 off_rec   u32 fin)   [offsets desde el inicio]
 *   entradas (nprims x 4 B): { u8 id (1..nprims), u8 flags (bit 7 = semitransparente resta), u16 off desde off_prims }
 *   primitiva en off_prims+off:   u32 tag | u8 r,g,b,código | pares (x,y) i16 ...
 *       código 0x28: polígono plano de 4 vértices (24 B): xy0 xy1 xy2 xy3 (tira: triángulos 0-1-2 y 1-2-3), negro opaco
 *       código 0x38/0x3A: polígono Gouraud de 4 vértices (36 B): xy0 rgb1 xy1 rgb2 xy2 rgb3 xy3; el 0x3A es
 *                         semitransparente con modo "resta": píxel = destino - color (blanco = negro total)
 *   off_idx: nprims x 4 B, por primitiva el índice de registro de cada vértice (0 = fijo)
 *   off_rec: registros de 4 B { u8 lo, u8 hi, i8 dx, i8 dy }: desplazamiento del vértice en el instante t de la
 *            animación de apertura: (t - hi) * (dx,dy) / 4 para t en (lo,hi]. Con lo=0, hi=8: el estado inicial
 *            (t = -1 en el original) es final + (lo-hi)*(dx,dy)/4 = final - 2*(dx,dy); a los 8 pasos queda el final.
 *   Coordenadas de pantalla del original en píxeles (x derecha, y abajo), lienzo = máx de los vértices.
 * El hueco de la máscara (la abertura) es el contorno interior de los polígonos planos que NO tocan el borde del
 * lienzo; las bandas Gouraud son el degradado interior de ese contorno.
 *
 * ---- Contenedor de stage ---- (igual que tools/stage y tools/wmodel/mdlfmt.py)
 */
#pragma once
#include <stddef.h>
#include <stdint.h>

#define BOX_MAX_OUT  128
#define BOX_MAX_BAND 96

typedef struct {
    float x, y;       /* posición final (píxeles de pantalla) */
    float sx, sy;     /* desplazamiento en el estado cerrado (t inicial): pos_cerrada = final + (sx,sy) */
} BoxVtx;

typedef struct { BoxVtx v[4]; float a[4]; } BoxBandQuad;   /* a = opacidad del negro 0..1 (resta -> alfa) */

typedef struct {
    int   canvas_w, canvas_h;          /* píxeles del lienzo del original */
    float cx, cy;                      /* centro del hueco final (px) */
    int   nout;  BoxVtx out[BOX_MAX_OUT];     /* contorno del hueco, convexo, ordenado por ángulo (y abajo) */
    int   nband; BoxBandQuad band[BOX_MAX_BAND];
    int   from_disc;                   /* 1 = leído del disco; 0 = abertura genérica de respaldo */
} BoxSight;

/* Abertura genérica (sin datos): rectángulo redondeado centrado. w,h,radio y banda en píxeles del lienzo. */
void box_sight_fallback(BoxSight *s, int canvas_w, int canvas_h, float cx, float cy,
                        float hole_w, float hole_h, float radius, float band);

/* Decodifica el recurso. 0 = bien; <0 = formato (-1 tamaño/cabecera, -2 offsets, -3 pocos vértices). */
int box_sight_parse(const uint8_t *b, size_t n, BoxSight *out);

/* Busca un fichero del stage: id16 + tipo (carácter, p.ej. 's','k','p'). 0 = bien; -1 stage/fichero no encontrado,
 * -2 formato. El puntero apunta dentro de 'dir' (no se copia). */
int box_stage_find(const uint8_t *dir, size_t dir_len, const char *stage, uint16_t id16, uint8_t type,
                   const uint8_t **p, size_t *n);

/* Imagen de textura (PCX RLE de 8 o 4 bits con paleta) -> RGBA8 (malloc; liberar con free). 0 = bien. */
int box_tex_decode(const uint8_t *b, size_t n, int *w, int *h, uint8_t **rgba);
