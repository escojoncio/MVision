#pragma once
/* ns_cpuref: rasterizador de REFERENCIA por CPU para el renderer nativo (sin GPU). Dibuja triángulos en espacio de mundo con la
 * semántica de la tarjeta gráfica original: VRAM de 1024x512 de 16 bits, texturas de 4/8/15 bits con CLUT, ventana de textura, texel
 * 0 transparente, color por vértice modulado como el hardware (texel*color/128), los 4 modos de semitransparencia (el bit 15 del
 * texel decide si un texel texturizado es semitransparente), z-buffer para lo opaco y semitransparentes ordenados de atrás adelante.
 * Es independiente de ns_types.h (el contrato de escena): un adaptador convierte NativeScene -> NsRTri. NO forma parte de la app
 * (herramienta/validador de escritorio; el directorio vr/native no está en la lista cerrada de app/stage.sh).
 *
 * Convención del ojo: X derecha, Y abajo, Z delante (como el espacio de vista del motor); e = A*p + t, A fila-mayor.
 * Los colores internos van en unidades de 5 bits por canal (0..31) como la memoria de imagen original. */
#include <stdint.h>
#include <stddef.h>

#define NS_TF_TEXTURED 1u   /* hay textura (tpage/clut/uv) */
#define NS_TF_SEMI     2u   /* primitiva semitransparente (modo abr); en texturizadas solo los texels con bit15=1 se mezclan */
#define NS_TF_RAW      4u   /* texturizada sin modulación por el color del vértice */

typedef struct { float p[3]; float u, v; uint8_t r, g, b, pad; } NsRVert;   /* uv en texels 0..255; color 128 = neutro */
/* tpage: bits 0-3 x/64, 4 y/256, 5-6 abr (informativo), 7-8 modo de color (0=4b, 1=8b, 2/3=15b). clut: bits 0-5 x/16, 6-14 y.
 * tw: ventana de textura empaquetada mx | my<<8 | ox<<16 | oy<<24 (unidades de 8 texels; 0 = sin ventana). */
typedef struct { NsRVert v[3]; uint16_t tpage, clut; uint8_t flags, abr; uint16_t pad; uint32_t tw; } NsRTri;
typedef char ns_check_tri_size[(sizeof(NsRTri) == 84) ? 1 : -1];

typedef struct { const NsRTri *tris; size_t ntris; const uint16_t *vram; /* 1024*512 */ } NsRScene;

typedef struct {
    float A[9], t[3];            /* mundo -> ojo: e = A*p + t */
    float fx, fy, cx, cy;        /* proyección en píxeles: sx = cx + fx*x/z, sy = cy + fy*y/z */
    int w, h;
    float znear;                 /* plano cercano en unidades del ojo (>0) */
    float zreject;               /* > 0: plano z = H/2 de la GTE: el vértice con z < zreject se proyecta en z = zreject (la división satura en 2,0) y solo se descarta el triángulo sin ningún vértice a z > 0; es lo que hace el motor con la cara ENTERA sin partir (ns_gpu_gte_tri); el sombreador ya no lo aplica (reglas por fragmento, ns_gpu_engine_tri). 0 = sin ajuste */
} NsRCam;

/* Búfer de color (0..31 por canal) + 1/z (0 = sin cobertura: hueco). */
typedef struct { int w, h; float *c; float *iz; } NsRBuf;

#define NS_O_FLOAT   1u   /* sin cuantizar a 5 bits (suave); por defecto se cuantiza como el hardware */
#define NS_O_1THREAD 2u   /* no usar hilos en cubemap/panorama */

typedef struct {
    size_t tris_in; size_t tris_drawn;     /* triángulos de entrada / que tocaron algún píxel (suma de caras en cubemap) */
    size_t px_opaque, px_semi;             /* escrituras de píxel */
    double hole_frac;                      /* fracción de píxeles sin cobertura de la salida */
    double ms;
} NsRStats;

/* --- cámaras --- */
/* A partir de la posición y la base de la cámara en el mundo (right, down, fwd; ortonormal) y el FOV horizontal (grados). */
void ns_cam_basis(NsRCam *c, const float pos[3], const float right[3], const float down[3], const float fwd[3], float fov_h_deg, int w, int h);
/* Mundo con Y hacia abajo (como el motor): yaw sobre Y (0 = +Z, positivo hacia +X), pitch positivo = mirar arriba (-Y), roll en rad = 0. */
void ns_cam_yaw_pitch(NsRCam *c, const float pos[3], float yaw_deg, float pitch_deg, float fov_h_deg, int w, int h);
/* Cámara del motor: e = A*p + t (A fila-mayor, ya en unidades de mundo), distancia de proyección H y centro OFX/OFY en la pantalla de
 * referencia ref_w x ref_h (p. ej. 320x240); la salida w x h escala la proyección (w/ref_w, h/ref_h). */
void ns_cam_game(NsRCam *c, const float A[9], const float t[3], float H, float ofx, float ofy, int ref_w, int ref_h, int w, int h);

/* --- render --- */
int ns_buf_alloc(NsRBuf *b, int w, int h);       /* 0 = bien */
void ns_buf_free(NsRBuf *b);
void ns_buf_clear(NsRBuf *b, float r5, float g5, float b5);
/* Una vista en perspectiva. out debe estar reservado con (cam->w, cam->h). */
void ns_render_view(const NsRScene *s, const NsRCam *cam, uint32_t opts, NsRBuf *out, NsRStats *st);
/* Cubemap de 6 caras NxN centradas en la pose de pose (solo se usan A y t). Orden: delante, derecha, atrás, izquierda, arriba, abajo
 * (respecto al ojo de pose). faces[i] reservadas con (n, n). */
void ns_render_cube(const NsRScene *s, const NsRCam *pose, int n, float znear, uint32_t opts, NsRBuf faces[6], NsRStats *st);
/* Panorama equirectangular 360x180 (centro = delante) a partir de un cubemap. out reservado con (w, h). */
void ns_cube_to_equirect(const NsRBuf faces[6], const NsRCam *pose, NsRBuf *out);
void ns_render_pano(const NsRScene *s, const NsRCam *pose, float znear, uint32_t opts, NsRBuf *out, NsRStats *st);
/* Cubemap en cruz (4x3 caras; huecos en hole_rgb 0xRRGGBB o negro). rgb de 4n x 3n. */
void ns_cube_cross_rgb8(const NsRBuf faces[6], uint32_t hole_rgb, int mark_holes, uint8_t *rgb);

/* --- salida --- */
/* Convierte a RGB8. Si mark_holes, los píxeles sin cobertura salen en hole_rgb (0xRRGGBB); si no, en el color de fondo del búfer. */
void ns_buf_to_rgb8(const NsRBuf *b, uint8_t *rgb, int mark_holes, uint32_t hole_rgb);
int ns_write_png(const char *path, int w, int h, const uint8_t *rgb);    /* PNG sin compresión; 0 = bien */
int ns_write_ppm(const char *path, int w, int h, const uint8_t *rgb);
uint8_t *ns_read_ppm(const char *path, int *w, int *h);                  /* P6 8 bits; free() */

typedef struct { double mae; double frac_over; int thr; long npix; } NsRDiff;   /* error absoluto medio (0..255) y fracción de píxeles con |dif|>thr en algún canal */
/* Compara dos RGB8 del mismo tamaño; si diff != NULL escribe una imagen de diferencia (3*w*h). */
void ns_diff_rgb8(const uint8_t *a, const uint8_t *b, int w, int h, int thr, uint8_t *diff, NsRDiff *out);

/* Muestreo suelto de la VRAM como el hardware (para pruebas): devuelve la palabra de 16 bits del texel (u,v) (antes de modular). */
uint16_t ns_sample_texel(const uint16_t *vram, uint16_t tpage, uint16_t clut, uint32_t tw, int u, int v);
