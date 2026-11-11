#pragma once
/* vr_mosaic: el mosaico de censura del juego (actor mosaic.c) como parche del MUNDO. Función pura (C99, sin plataforma); la usa vrhost.c y la prueba test_mosaic.c.
 *
 * Qué dibuja el original (medido en volcados): una rejilla de 4x4 polígonos de 6x6 píxeles, sin posición de vista (2D), texturizados con el PROPIO fotograma que se está pintando y con
 * las cuatro esquinas en el mismo texel: cada celda sale del color liso de un píxel de lo ya dibujado debajo. Ese texel está DENTRO de la propia celda (fila superior), así que la celda
 * se pisa su textura y la captura no lo resuelve (GpuOp.fbk = 0 en los volcados reales): el color de la celda se lee de la VRAM al cerrar la lista, en la propia celda, que es lo que la
 * PS1 dejó pintado. Si la captura sí lo resolvió (fbk), vale su texel. Es 2D de pantalla, así que en VR iba al panel 2D: un parche flotando ante la cara, sin tapar al personaje.
 *
 * Qué se hace: cada celda pasa a ser un cuadrilátero liso en el mundo del motor, colocado sobre lo que el juego dibujó debajo del mosaico: se busca, entre los polígonos 3D de la MISMA
 * lista, el más cercano bajo el centro del mosaico (prueba de punto en triángulo con profundidad interpolada en 1/z) y las esquinas de cada celda se desproyectan a esa profundidad con la
 * proyección MEDIDA en la propia lista (ajuste por mínimos cuadrados x = cx + hx * vx/vz, y = cy + hy * vy/vz sobre sus vértices 3D: sin suponer H, centro ni factor vertical). El render
 * nativo lo dibuja como un efecto más (NsEffect) y las primitivas originales salen del panel 2D (clase VPC_WORLD). */
#include <math.h>
#include <stdint.h>
#include <string.h>
#include "gpu.h"

#define VRMOSAIC_MAX 64          /* celdas (el original usa 16) */
#define VRMOSAIC_NEAR 0.97f      /* las celdas van un 3 % por delante de la superficie tapada (sin pelea de profundidad) */

typedef struct {
    int n;                        /* celdas; 0 = sin mosaico */
    float p[VRMOSAIC_MAX][4][3];  /* esquinas en el mundo del motor, en el orden del paquete (0,1,2,3 = Z) */
    uint8_t rgb[VRMOSAIC_MAX][3]; /* color liso de la celda, 0..255 */
    float depth;                  /* profundidad de vista de la superficie tapada (unidades del motor) */
} VrMosaic;

/* Profundidad de vista del triángulo (a,b,c) en el píxel (px,py), o 0 si el punto queda fuera (o el triángulo no vale). 1/z es lineal en pantalla. */
static inline float vrmosaic_tri_depth(const GpuVtx *a, const GpuVtx *b, const GpuVtx *c, float px, float py) {
    float x0 = a->x, y0 = a->y, x1 = b->x, y1 = b->y, x2 = c->x, y2 = c->y;
    float d = (y1 - y2) * (x0 - x2) + (x2 - x1) * (y0 - y2);
    if (fabsf(d) < 1e-6f) return 0.0f;
    float w0 = ((y1 - y2) * (px - x2) + (x2 - x1) * (py - y2)) / d, w1 = ((y2 - y0) * (px - x2) + (x0 - x2) * (py - y2)) / d, w2 = 1.0f - w0 - w1;
    if (w0 < 0.0f || w1 < 0.0f || w2 < 0.0f) return 0.0f;
    float iz = w0 / a->vz + w1 / b->vz + w2 / c->vz;
    return iz > 0.0f ? 1.0f / iz : 0.0f;
}

/* ¿Es la primitiva una celda del mosaico que el parche sabe reproducir? Cuadrilátero texturizado con color conocido: texel resuelto por la captura (fbk) o VRAM de cierre de la lista. */
static inline int vrmosaic_is_cell(const GpuOp *o, const uint16_t *vram) {
    return o->type == GPU_OP_POLY && o->nv == 4 && (o->flags & GPU_F_TEXTURED) && (o->fbk || vram != NULL);
}
/* Celda que no pinta nada: la captura resolvió un texel transparente. Es celda (no estropea el parche) pero no se emite. */
static inline int vrmosaic_cell_empty(const GpuOp *o) { return o->fbk && o->fbt == 0; }

/* Construye el parche a partir de la lista. `is_mosaic[i]` != 0 marca las primitivas del mosaico (procedencia). `vram` (1024x512, puede ser NULL): VRAM al CERRAR la lista (GpuDrawList.vram_end),
 * de donde sale el color de las celdas que la captura no resolvió. `eye` / `M` (vista -> mundo, fila-mayor, columnas X derecha, Y abajo, Z delante): la cámara con la que se dibujó la lista.
 * Devuelve el número de celdas (0 si no hay mosaico, no hay geometría 3D debajo o la proyección no se puede medir); `out` queda siempre definido. */
static inline int vrmosaic_build(const GpuOp *ops, uint32_t nops, const uint8_t *is_mosaic, const uint16_t *vram, const float eye[3], const float M[9], VrMosaic *out) {
    memset(out, 0, sizeof *out);
    if (!ops || !is_mosaic || !eye || !M) return 0;
    /* 1) celdas y su centro */
    float bx0 = 1e9f, by0 = 1e9f, bx1 = -1e9f, by1 = -1e9f; int nc = 0;
    for (uint32_t i = 0; i < nops; i++) {
        const GpuOp *o = &ops[i];
        if (!is_mosaic[i] || !vrmosaic_is_cell(o, vram) || vrmosaic_cell_empty(o)) continue;
        for (int k = 0; k < 4; k++) { float x = o->v[k].x, y = o->v[k].y; if (x < bx0) bx0 = x; if (x > bx1) bx1 = x; if (y < by0) by0 = y; if (y > by1) by1 = y; }
        nc++;
    }
    if (!nc) return 0;
    const float mx = 0.5f * (bx0 + bx1), my = 0.5f * (by0 + by1);
    /* 2) proyección medida y profundidad de lo dibujado debajo del centro */
    double n = 0, sa = 0, saa = 0, sx = 0, sax = 0, sb = 0, sbb = 0, sy = 0, sby = 0;
    float zc = 0.0f;
    for (uint32_t i = 0; i < nops; i++) {
        const GpuOp *o = &ops[i];
        if (is_mosaic[i] || o->type != GPU_OP_POLY || o->nv < 3) continue;
        int ok = 1;
        for (int k = 0; k < o->nv; k++) if (!o->v[k].vvalid || o->v[k].vflags || !(o->v[k].vz > 1.0f) || !isfinite(o->v[k].vx) || !isfinite(o->v[k].vy)) ok = 0;
        if (!ok) continue;
        for (int k = 0; k < o->nv; k++) {
            double a = (double)o->v[k].vx / o->v[k].vz, b = (double)o->v[k].vy / o->v[k].vz;
            n += 1; sa += a; saa += a * a; sx += o->v[k].x; sax += a * o->v[k].x; sb += b; sbb += b * b; sy += o->v[k].y; sby += b * o->v[k].y;
        }
        float z = vrmosaic_tri_depth(&o->v[0], &o->v[1], &o->v[2], mx, my);
        if (!(z > 0.0f) && o->nv == 4) z = vrmosaic_tri_depth(&o->v[1], &o->v[2], &o->v[3], mx, my);
        if (z > 0.0f && (zc == 0.0f || z < zc)) zc = z;
    }
    double da = n * saa - sa * sa, db = n * sbb - sb * sb;
    if (n < 12 || !(zc > 0.0f) || !(da > 1e-9) || !(db > 1e-9)) return 0;
    const double hx = (n * sax - sa * sx) / da, cx = (sx - hx * sa) / n, hy = (n * sby - sb * sy) / db, cy = (sy - hy * sb) / n;
    if (!(fabs(hx) > 8.0) || !(fabs(hy) > 8.0)) return 0;
    /* 3) celdas al mundo */
    const float z = zc * VRMOSAIC_NEAR;
    for (uint32_t i = 0; i < nops && out->n < VRMOSAIC_MAX; i++) {
        const GpuOp *o = &ops[i];
        if (!is_mosaic[i] || !vrmosaic_is_cell(o, vram) || vrmosaic_cell_empty(o)) continue;
        int c = out->n++;
        for (int k = 0; k < 4; k++) {
            float v[3] = {(float)(((double)o->v[k].x - cx) / hx) * z, (float)(((double)o->v[k].y - cy) / hy) * z, z};
            for (int r = 0; r < 3; r++) out->p[c][k][r] = eye[r] + M[3 * r] * v[0] + M[3 * r + 1] * v[1] + M[3 * r + 2] * v[2];
        }
        if (o->fbk) {
            /* color: texel resuelto (RGB555) modulado por el color del vértice, como la GPU: (texel5 * color) >> 4 por canal, saturado; textura cruda: el texel tal cual */
            for (int ch = 0; ch < 3; ch++) {
                int t5 = (o->fbt >> (5 * ch)) & 31, col = ch == 0 ? o->v[0].r : ch == 1 ? o->v[0].g : o->v[0].b;
                int v5 = (o->flags & GPU_F_RAW) ? t5 : (t5 * col) >> 7;
                if (v5 > 31) v5 = 31;
                out->rgb[c][ch] = (uint8_t)((v5 << 3) | (v5 >> 2));
            }
        } else {
            /* color: lo que la PS1 dejó pintado en la celda (ya modulado), leído en su centro */
            int px = (o->v[0].x + o->v[1].x + o->v[2].x + o->v[3].x) / 4, py = (o->v[0].y + o->v[1].y + o->v[2].y + o->v[3].y) / 4;
            if (px < o->cx1) px = o->cx1;
            if (px > o->cx2) px = o->cx2;
            if (py < o->cy1) py = o->cy1;
            if (py > o->cy2) py = o->cy2;
            const uint16_t w = (px >= 0 && px < 1024 && py >= 0 && py < 512) ? vram[py * 1024 + px] : 0;
            for (int ch = 0; ch < 3; ch++) { int v5 = (w >> (5 * ch)) & 31; out->rgb[c][ch] = (uint8_t)((v5 << 3) | (v5 >> 2)); }
        }
    }
    out->depth = zc;
    return out->n;
}
