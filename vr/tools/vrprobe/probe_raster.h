#pragma once
/* probe_raster: rasterizador de cobertura por CPU para el probador de VR (vr/tools/vrprobe). Proyecta triángulos del "mundo VR" de vr_scene
 * por ojo con la misma cadena que la app (vista = inversa de cabeza * desplazamiento del ojo; el mundo VR se usa tal cual como espacio del
 * usuario), a baja resolución, solo para medir cobertura (geometría) y no para ver la imagen final. Más un escritor de PNG sin compresión. */
#include <stdint.h>

typedef struct { int w, h; float tx, ty, near; } PrCam;       /* tx, ty = tangentes de la mitad del campo de visión */
typedef struct { float R[9]; float p[3]; } PrEye;             /* pc = R * (pw - p): X derecha, Y arriba, Z atrás (delante = -Z) */
typedef struct { int w, h; float *z; int *id; int cur; } PrBuf;   /* id (opcional, w*h): índice del triángulo que dejó cada píxel (cur = el que se rasteriza) */                 /* z = profundidad más cercana por píxel (1e30 = fondo) */

/* Ojo a partir de la pose de la cabeza (16 floats columna-mayor: X der, Y arriba, Z atrás, posición). side: -1 izquierdo, +1 derecho; ipd en m. */
void pr_eye_from_head(const float head[16], int side, float ipd, PrEye *e);
void pr_buf_init(PrBuf *b, int w, int h);
void pr_buf_free(PrBuf *b);
void pr_buf_clear(PrBuf *b);
/* Rasteriza un triángulo (v = 9 floats, mundo VR) a doble cara. Devuelve el nº de píxeles nuevos escritos más los que ya estaban (tocados);
 * 0 si no toca ninguno (fuera de campo, detrás, degenerado). */
int pr_tri(PrBuf *b, const PrCam *c, const PrEye *e, const float *v);
/* Proyecta un punto del mundo VR. Devuelve 1 si queda delante del ojo (px, py en píxeles, depth en m). */
int pr_project(const PrCam *c, const PrEye *e, const float p[3], float *px, float *py, float *depth);

/* PNG RGB8 (sin compresión: bloques "stored" de deflate). 0 = bien. */
int pr_write_png(const char *path, int w, int h, const uint8_t *rgb);
/* Texto mínimo (dígitos, letras mayúsculas básicas, . % - : espacio) en un búfer RGB; escala entera. */
void pr_text(uint8_t *rgb, int w, int h, int x, int y, int scale, const char *s, uint8_t r, uint8_t g, uint8_t b);
void pr_rect(uint8_t *rgb, int w, int h, int x, int y, int rw, int rh, uint8_t r, uint8_t g, uint8_t b);
void pr_line(uint8_t *rgb, int w, int h, int x0, int y0, int x1, int y1, uint8_t r, uint8_t g, uint8_t b);
