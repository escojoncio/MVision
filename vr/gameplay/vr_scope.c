/* vr_scope: ver vr_scope.h. */
#include <math.h>
#include "vr_scope.h"

#define CX 160.0f
#define CY 128.0f
#define AX 127.0f       /* semiejes de la abertura medida (px del lienzo) */
#define AY 125.0f
#define AP 2.6f         /* exponente del superelipse (1 = rombo, 2 = elipse; el perfil medido es algo más cuadrado que una elipse) */

float vrscope_fov_y(float zoom) {
    if (!(zoom >= 1.0f) || zoom > 100.0f) zoom = VRSCOPE_ZOOM;
    return 2.0f * atanf((VRSCOPE_CANVAS_H * 0.5f) / (VRSCOPE_BASE_H * zoom));
}

void vrscope_canvas_to_uv(float cx, float cy, float *u, float *v) {
    if (u) *u = (cx - (CX - VRSCOPE_CANVAS_H * 0.5f)) / VRSCOPE_CANVAS_H;
    if (v) *v = cy / VRSCOPE_CANVAS_H;
}

static float edge(float cx, float cy) {      /* < 1 dentro de la abertura */
    return powf(fabsf(cx - CX) / AX, AP) + powf(fabsf(cy - CY) / AY, AP);
}
int vrscope_inside(float cx, float cy) { return edge(cx, cy) < 1.0f; }

int vrscope_on_reticle(float cx, float cy) {
    float dx = fabsf(cx - CX), dy = fabsf(cy - CY);
    /* cruz fina: brazos de 64 px (1,2 px de grosor) */
    if (dy < 0.6f && dx <= 64.0f) return 1;
    if (dx < 0.6f && dy <= 64.0f) return 1;
    /* postes gruesos: de 67 px al borde del lienzo (5 px de grosor; los laterales 6 px) */
    if (dx < 2.5f && dy >= 67.0f) return 1;
    if (dy < 3.0f && dx >= 66.0f) return 1;
    return 0;
}

void vrscope_draw_overlay(uint32_t *rgba, int size) {
    if (!rgba || size < 16 || size > 2048) return;
    const float k = VRSCOPE_CANVAS_H / (float)size;     /* px de lienzo por texel */
    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++) {
            /* 2 x 2 muestras por texel: bordes de la abertura y líneas finas sin saltos */
            int ink = 0, out = 0;
            for (int sy = 0; sy < 2; sy++) for (int sx = 0; sx < 2; sx++) {
                float cx = (CX - VRSCOPE_CANVAS_H * 0.5f) + ((float)x + 0.25f + 0.5f * (float)sx) * k;
                float cy = ((float)y + 0.25f + 0.5f * (float)sy) * k;
                if (!vrscope_inside(cx, cy)) out++;
                else if (vrscope_on_reticle(cx, cy)) ink++;
            }
            uint32_t px;
            if (out >= 3) px = 0xFF000000u;                                    /* fuera de la abertura */
            else if (ink + out >= 1) {
                /* mezcla: negro con alfa proporcional a las muestras tapadas (retícula casi negra, 8 8 10) */
                uint32_t a = (uint32_t)((ink + out) * 255 / 4);
                px = (a << 24) | (10u << 16) | (8u << 8) | 8u;
            } else px = 0;
            rgba[y * size + x] = px;
        }
    }
}
