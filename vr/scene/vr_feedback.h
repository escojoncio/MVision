#pragma once
/* vr_feedback: efectos de REALIMENTACIÓN de pantalla del juego (blur.c, blurpure.c, focus.c y cualquier otro actor que haga lo mismo). Funciones puras (C99, sin plataforma);
 * las usan vr_prov.c (clase de la primitiva) y vrhost.c (modelo del efecto) y las prueba test_feedback.c.
 *
 * Qué hace el original (medido en volcados): después del 3D, el actor pinta sprites o polígonos que cubren la imagen, texturizados en color directo (15 bits) con un FRAME BUFFER:
 *  - el OTRO búfer del doble búfer (el fotograma anterior, ya mostrado), semitransparente: estela de movimiento. blurpure.c: 2 sprites de 160x184, (B+F)/2, color 120/128, sin deformar.
 *    blur.c: rejilla de 2x2 polígonos, (B+F)/2, con la fuente algo más pequeña que el destino (ampliación) y el vértice central desplazado (deformación);
 *  - el MISMO búfer que se está dibujando, desplazado: focus.c, 2 sprites de 160x256 dos píxeles a la izquierda.
 * Son 2D de pantalla, así que iban al panel 2D, que los pintaba con la VRAM: una copia PLANA y de baja resolución del render de la PS1 encima de la escena nativa en 3D.
 *
 * Qué se hace: esas primitivas salen del panel y el efecto se reproduce sobre la imagen nativa de la pantalla con el mismo modelo, dentro de la zona que cubren:
 *     salida = a * actual + b * fuente(M * p)
 * (a, b) = modo de mezcla y color del sprite (opaco: a = 0); fuente = la imagen anterior de la pantalla o la actual; M = afín destino -> fuente ajustada por mínimos cuadrados a los
 * vértices (identidad, desplazamiento, ampliación). Coordenadas normalizadas de la imagen de la pantalla 3D (ns_cinema_window: 320 x 240 px del motor alrededor del centro de proyección):
 * u = 0,5 + (x - cx) / 320, v = 0,5 + (y - cy) / 240, v hacia abajo, con (cx, cy) = desplazamiento de dibujo de las primitivas 3D de la lista (su centro de proyección). */
#include <math.h>
#include <stdint.h>
#include <string.h>
#include "gpu.h"

#define VRFB_STRIP_MIN_W 16
enum { VRFB_NONE = 0, VRFB_PREV = 1, VRFB_SELF = 2 };   /* fuente: ninguna / el otro búfer (fotograma anterior) / el búfer que se dibuja (mismo fotograma) */
#define VRFB_IMG_W 320.0f
#define VRFB_IMG_H 240.0f
#define VRFB_MAX_ERR_PX 12.0f     /* residuo máximo del ajuste afín (px del motor): la rejilla deformada de blur.c se aparta ~5 px */
#define VRFB_MIN_AREA 0.2f        /* fracción mínima de la imagen que deben cubrir (un sprite pequeño que muestrea la pantalla no es este efecto) */
/* Ondulación del agua de la intro (takabe/wt_view.c): tiras SPRT de 160 x 2 (o 1) filas que releen el MISMO búfer con la fila de origen desplazada v0 = fila + rsin(i * 128) / 1024 (+-4 filas, periodo de 32 tiras = 64 filas
 * del motor, la fase avanza con el reloj). No cabe en una afín: se ajusta aparte como v' += wave[0] * sin(K (v - 0,5)) + wave[1] * cos(K (v - 0,5)), K = 2 pi * 240 / 64 rad por unidad de v. */
#define VRFB_WAVE_K (2.0f * 3.14159265f * VRFB_IMG_H / 64.0f)
#define VRFB_WAVE_MIN_STRIPS 16   /* tiras mínimas para intentar el ajuste de la ondulación */
#define VRFB_WAVE_MAX_ERR_PX 2.0f /* residuo máximo (filas del motor) tras restar la ondulación: la tabla de wt_view es entera; medido en la intro (auto01, auto02): 1,2-1,4 */
#define VRFB_WAVE_CAP 2048        /* vértices de tira admitidos en el ajuste (228 tiras x 4 = 912 en la intro) */

typedef struct {
    int n;             /* primitivas de realimentación candidatas de la lista */
    int mode;          /* VRFB_PREV / VRFB_SELF; VRFB_NONE = no hay o no encajan en el modelo (siguen en el panel 2D) */
    float a, b[3];     /* salida = a * actual + b[c] * fuente */
    float m[6];        /* fuente: u' = m[0] u + m[1] v + m[2]; v' = m[3] u + m[4] v + m[5] */
    float wave[2];     /* ondulación vertical que se suma a v' (ver VRFB_WAVE_K): wave[0] sin + wave[1] cos de K (v - 0,5), en unidades de v; 0, 0 = sin ondulación */
    float rect[4];     /* zona afectada u0, v0, u1, v1 (recortada a 0..1) */
    float err_px;      /* residuo máximo del ajuste (px del motor) */
} VrFeedback;

/* ¿La primitiva muestrea un frame buffer? Texturizada en color directo, sin ventana de textura ni volteo, no resuelta ya por la captura (fbk: un solo texel, la pinta el panel como color
 * liso) y con TODA su fuente dentro del área de dibujo (VRFB_SELF) o del OTRO búfer del doble búfer (VRFB_PREV). `other` = {x, y, w, h} del otro búfer: el área mostrada al cerrar la lista
 * ANTERIOR (lo que estaba en pantalla mientras se dibujaba esta). NULL o no válido (w o h <= 0, o solapa el área de dibujo): se deduce como la pareja del área de dibujo en una rejilla de
 * su mismo tamaño (búferes en x = 0 y x = W, o en y = 0 e y = H), nunca una zona cualquiera de la VRAM (texturas en color directo). org (opcional) = origen del búfer fuente. */
static inline int vrfb_op_source(const GpuOp *o, const int other[4], int org[2]) {
    if (!o || !(o->flags & GPU_F_TEXTURED) || o->tp < 2 || o->fbk || o->twmx || o->twmy || (o->flags & (GPU_F_FLIPX | GPU_F_FLIPY))) return VRFB_NONE;
    int u0 = 255, v0 = 255, u1 = 0, v1 = 0;
    if (o->type == GPU_OP_RECT) {
        if (o->rw < 2 || (o->rh < 2 && o->rw < VRFB_STRIP_MIN_W)) return VRFB_NONE;     /* una tira de 1 fila (wt_view.c) vale si es ancha; un sprite diminuto no */
        u0 = o->v[0].u; v0 = o->v[0].v; u1 = u0 + o->rw - 1; v1 = v0 + o->rh - 1;
    } else if (o->type == GPU_OP_POLY && (o->nv == 3 || o->nv == 4)) {
        for (int k = 0; k < o->nv; k++) {
            if (o->v[k].u < u0) u0 = o->v[k].u;
            if (o->v[k].u > u1) u1 = o->v[k].u;
            if (o->v[k].v < v0) v0 = o->v[k].v;
            if (o->v[k].v > v1) v1 = o->v[k].v;
        }
        if (u1 - u0 < 2 && v1 - v0 < 2) return VRFB_NONE;     /* un solo texel (mosaico): no es una imagen */
    } else return VRFB_NONE;
    const int x0 = o->tx + u0, y0 = o->ty + v0, x1 = o->tx + u1, y1 = o->ty + v1;
    const int W = o->cx2 - o->cx1 + 1, H = o->cy2 - o->cy1 + 1;
    if (W < 16 || H < 16) return VRFB_NONE;
    if (x0 >= o->cx1 && y0 >= o->cy1 && x1 <= o->cx2 && y1 <= o->cy2) {
        if (org) { org[0] = o->cx1; org[1] = o->cy1; }
        return VRFB_SELF;
    }
    int bx[2], by[2], bw[2], bh[2], nb = 0;
    if (other && other[2] > 0 && other[3] > 0 && (other[0] + other[2] <= o->cx1 || other[0] > o->cx2 || other[1] + other[3] <= o->cy1 || other[1] > o->cy2)) {
        bx[0] = other[0]; by[0] = other[1]; bw[0] = other[2]; bh[0] = other[3]; nb = 1;
    } else {
        if (o->cx1 % W == 0) { bx[nb] = ((o->cx1 / W) & 1) ? o->cx1 - W : o->cx1 + W; by[nb] = o->cy1; bw[nb] = W; bh[nb] = H; nb++; }
        if (o->cy1 % H == 0) { bx[nb] = o->cx1; by[nb] = ((o->cy1 / H) & 1) ? o->cy1 - H : o->cy1 + H; bw[nb] = W; bh[nb] = H; nb++; }
    }
    for (int k = 0; k < nb; k++) {
        if (bx[k] < 0 || by[k] < 0 || bx[k] + bw[k] > 1024 || by[k] + bh[k] > 512) continue;
        if (x0 >= bx[k] && y0 >= by[k] && x1 < bx[k] + bw[k] && y1 < by[k] + bh[k]) {
            if (org) { org[0] = bx[k]; org[1] = by[k]; }
            return VRFB_PREV;
        }
    }
    return VRFB_NONE;
}

/* ¿Es una tira de la ondulación (sprite ancho de 1 o 2 filas que muestrea el mismo búfer)? El anfitrión, si una lista trae >= VRFB_WAVE_MIN_STRIPS, modela solo ellas (una lista con la onda y además un
 * desenfoque no cabe en un único modelo: el desenfoque se pierde, la onda no). */
static inline int vrfb_is_strip(const GpuOp *o) {
    return o && o->type == GPU_OP_RECT && o->rw >= VRFB_STRIP_MIN_W && o->rh >= 1 && o->rh <= 2 && vrfb_op_source(o, NULL, NULL) == VRFB_SELF;
}

/* Modelo del efecto de la lista. cand[i] != 0 marca las primitivas a considerar (el anfitrión: clase UNKNOWN por procedencia y vrfb_op_source != 0); NULL = todas las que muestrean un
 * frame buffer. other: ver vrfb_op_source. vram (opcional, 1024x512): VRAM al cerrar la lista, para saber si la fuente lleva el bit de máscara (los texels sin él se pintan opacos aunque el sprite sea
 * semitransparente); sin ella, un sprite semitransparente se da por mezclado. Devuelve out->mode; out queda siempre definido. Todas las candidatas deben compartir fuente, mezcla y color. */
static inline int vrfb_build(const GpuOp *ops, uint32_t nops, const uint8_t *cand, const int other[4], const uint16_t *vram, VrFeedback *out) {
    memset(out, 0, sizeof *out);
    out->m[0] = 1.0f; out->m[4] = 1.0f;
    if (!ops) return VRFB_NONE;
    /* Centro de la imagen de la pantalla 3D en píxeles de la lista (cx, cy), de la primera primitiva 3D. La ventana de la pantalla (ns_cinema_window) son los píxeles [0,320]x[0,240] de la
     * pantalla del motor con su centro de proyección en (OFX, OFY): si la GTE lleva OFX/OFY válidos, el origen de esa pantalla es el desplazamiento de dibujo (centro = dox + 160, doy + 120);
     * si no (este juego: OFX = OFY = 0 y el centro va en el desplazamiento de dibujo), la ventana va centrada en el centro de proyección (centro = dox, doy). */
    float cx = 0.0f, cy = 0.0f; int have_c = 0;
    for (uint32_t i = 0; i < nops && !have_c; i++) {
        const GpuOp *o = &ops[i];
        if (!(o->proj_h > 0.0f)) continue;
        const int gte_c = o->proj_ox > 0.0f && o->proj_ox < VRFB_IMG_W && o->proj_oy > 0.0f && o->proj_oy < VRFB_IMG_H;
        cx = (float)o->dox + (gte_c ? 0.5f * VRFB_IMG_W : 0.0f); cy = (float)o->doy + (gte_c ? 0.5f * VRFB_IMG_H : 0.0f); have_c = 1;
    }
    int mode = VRFB_NONE, semi = 0, abr = 0, raw = 0, col[3] = {0, 0, 0}, bad = 0;
    int sx0 = 0, sy0 = 0, sx1 = 0, sy1 = 0;            /* fuente de la primera candidata (VRAM), para medir el bit de máscara */
    double S[3][3] = {{0}}, bu[3] = {0}, bv[3] = {0};  /* ecuaciones normales del ajuste (compartidas por u' y v') */
    float r0 = 1e9f, r1 = 1e9f, r2 = -1e9f, r3 = -1e9f;
    /* dos pasadas: 0 acumula, 1 mide el residuo */
    double mu[3] = {1, 0, 0}, mv[3] = {0, 1, 0};
    float err = 0.0f, err_u = 0.0f, err_v = 0.0f;
    static float wx[VRFB_WAVE_CAP], wr[VRFB_WAVE_CAP];   /* muestras de la ondulación (hilo de emulación: una sola llamada a la vez) */
    int nw = 0, wave_ok = 1;
    for (int pass = 0; pass < 2 && !bad; pass++) {
        for (uint32_t i = 0; i < nops; i++) {
            const GpuOp *o = &ops[i];
            if (cand && !cand[i]) continue;
            int org[2] = {0, 0};
            const int src = vrfb_op_source(o, other, org);
            if (src == VRFB_NONE) { if (cand && pass == 0) { bad = 1; out->n++; } continue; }
            if (!have_c) { cx = (float)o->cx1 + 0.5f * (float)(o->cx2 - o->cx1 + 1); cy = (float)o->cy1 + 0.5f * (float)(o->cy2 - o->cy1 + 1); have_c = 1; }
            if (pass == 0) {
                const int sm = (o->flags & GPU_F_SEMI) != 0, rw = (o->flags & GPU_F_RAW) != 0;
                if (out->n == 0) {
                    mode = src; semi = sm; abr = o->abr; raw = rw; col[0] = o->v[0].r; col[1] = o->v[0].g; col[2] = o->v[0].b;
                } else if (src != mode || sm != semi || (sm && o->abr != abr) || rw != raw || (!rw && (o->v[0].r != col[0] || o->v[0].g != col[1] || o->v[0].b != col[2]))) bad = 1;
                out->n++;
            }
            /* pares destino -> fuente de sus vértices, en píxeles relativos a su búfer y luego normalizados */
            float d[4][2], s[4][2]; int nv;
            if (o->type == GPU_OP_RECT) {
                nv = 4;
                for (int k = 0; k < 4; k++) {
                    const float ox = (k & 1) ? (float)o->rw : 0.0f, oy = (k & 2) ? (float)o->rh : 0.0f;
                    d[k][0] = (float)o->v[0].x + ox; d[k][1] = (float)o->v[0].y + oy;
                    s[k][0] = (float)(o->tx + o->v[0].u) + ox; s[k][1] = (float)(o->ty + o->v[0].v) + oy;
                }
            } else {
                nv = o->nv;
                for (int k = 0; k < nv; k++) { d[k][0] = o->v[k].x; d[k][1] = o->v[k].y; s[k][0] = (float)(o->tx + o->v[k].u); s[k][1] = (float)(o->ty + o->v[k].v); }
            }
            if (pass == 0 && out->n == 1) {
                sx0 = sy0 = 1 << 20; sx1 = sy1 = -(1 << 20);
                for (int k = 0; k < nv; k++) { if ((int)s[k][0] < sx0) sx0 = (int)s[k][0]; if ((int)s[k][0] > sx1) sx1 = (int)s[k][0]; if ((int)s[k][1] < sy0) sy0 = (int)s[k][1]; if ((int)s[k][1] > sy1) sy1 = (int)s[k][1]; }
            }
            for (int k = 0; k < nv; k++) {
                const double pu = 0.5 + ((double)d[k][0] - cx) / VRFB_IMG_W, pv = 0.5 + ((double)d[k][1] - cy) / VRFB_IMG_H;
                /* la fuente, llevada al búfer de dibujo (misma posición relativa dentro de su búfer) */
                const double qu = 0.5 + ((double)s[k][0] - org[0] + o->cx1 - cx) / VRFB_IMG_W, qv = 0.5 + ((double)s[k][1] - org[1] + o->cy1 - cy) / VRFB_IMG_H;
                if (pass == 0) {
                    const double p[3] = {pu, pv, 1.0};
                    for (int a = 0; a < 3; a++) { for (int b = 0; b < 3; b++) S[a][b] += p[a] * p[b]; bu[a] += p[a] * qu; bv[a] += p[a] * qv; }
                    if ((float)pu < r0) r0 = (float)pu;
                    if ((float)pv < r1) r1 = (float)pv;
                    if ((float)pu > r2) r2 = (float)pu;
                    if ((float)pv > r3) r3 = (float)pv;
                } else {
                    const float eu = (float)fabs(mu[0] * pu + mu[1] * pv + mu[2] - qu) * VRFB_IMG_W, ev = (float)fabs(mv[0] * pu + mv[1] * pv + mv[2] - qv) * VRFB_IMG_H;
                    if (eu > err) err = eu;
                    if (ev > err) err = ev;
                    if (eu > err_u) err_u = eu;
                    if (ev > err_v) err_v = ev;
                    if (o->type == GPU_OP_RECT && o->rw >= VRFB_STRIP_MIN_W && o->rh <= 2) {      /* muestra de la ondulación: fila de destino y lo que la fuente se aparta de la afín */
                        if (nw < VRFB_WAVE_CAP) { wx[nw] = (float)(pv - 0.5); wr[nw] = (float)(qv - (mv[0] * pu + mv[1] * pv + mv[2])); nw++; } else wave_ok = 0;
                    } else wave_ok = 0;
                }
            }
        }
        if (pass == 0) {
            if (out->n == 0 || bad) break;
            /* resolver S m = b (Cramer); S singular (puntos alineados) = no hay imagen que ajustar */
            const double det = S[0][0] * (S[1][1] * S[2][2] - S[1][2] * S[2][1]) - S[0][1] * (S[1][0] * S[2][2] - S[1][2] * S[2][0]) + S[0][2] * (S[1][0] * S[2][1] - S[1][1] * S[2][0]);
            if (!(fabs(det) > 1e-9)) { bad = 1; break; }
            for (int w = 0; w < 2; w++) {
                const double *b = w ? bv : bu; double *m = w ? mv : mu;
                m[0] = (b[0] * (S[1][1] * S[2][2] - S[1][2] * S[2][1]) - S[0][1] * (b[1] * S[2][2] - S[1][2] * b[2]) + S[0][2] * (b[1] * S[2][1] - S[1][1] * b[2])) / det;
                m[1] = (S[0][0] * (b[1] * S[2][2] - S[1][2] * b[2]) - b[0] * (S[1][0] * S[2][2] - S[1][2] * S[2][0]) + S[0][2] * (S[1][0] * b[2] - b[1] * S[2][0])) / det;
                m[2] = (S[0][0] * (S[1][1] * b[2] - b[1] * S[2][1]) - S[0][1] * (S[1][0] * b[2] - b[1] * S[2][0]) + b[0] * (S[1][0] * S[2][1] - S[1][1] * S[2][0])) / det;
            }
        }
    }
    /* ondulación del agua: solo si TODAS las candidatas son tiras (>= VRFB_WAVE_MIN_STRIPS) y la afín deja un residuo vertical grande. Mínimos cuadrados de (sin, cos) con K fijo; vale si lo que queda es <= VRFB_WAVE_MAX_ERR_PX. */
    if (!bad && wave_ok && out->n >= VRFB_WAVE_MIN_STRIPS && nw > 0 && err_v > VRFB_WAVE_MAX_ERR_PX) {
        double ss = 0, cc = 0, sc = 0, rs = 0, rc = 0;
        for (int i = 0; i < nw; i++) {
            const double sn = sin((double)VRFB_WAVE_K * wx[i]), cs = cos((double)VRFB_WAVE_K * wx[i]);
            ss += sn * sn; cc += cs * cs; sc += sn * cs; rs += wr[i] * sn; rc += wr[i] * cs;
        }
        const double det = ss * cc - sc * sc;
        if (fabs(det) > 1e-6) {
            const double as = (rs * cc - rc * sc) / det, ac = (rc * ss - rs * sc) / det;
            float ew = 0.0f;
            for (int i = 0; i < nw; i++) {
                const float e = (float)fabs(wr[i] - as * sin((double)VRFB_WAVE_K * wx[i]) - ac * cos((double)VRFB_WAVE_K * wx[i])) * VRFB_IMG_H;
                if (e > ew) ew = e;
            }
            if (isfinite(as) && isfinite(ac) && ew <= VRFB_WAVE_MAX_ERR_PX) { out->wave[0] = (float)as; out->wave[1] = (float)ac; err = err_u > ew ? err_u : ew; }
        }
    }
    out->err_px = err;
    if (out->n == 0 || bad || !(err <= VRFB_MAX_ERR_PX)) return VRFB_NONE;
    for (int k = 0; k < 3; k++) { if (!isfinite(mu[k]) || !isfinite(mv[k])) return VRFB_NONE; out->m[k] = (float)mu[k]; out->m[3 + k] = (float)mv[k]; }
    out->rect[0] = r0 < 0.0f ? 0.0f : r0; out->rect[1] = r1 < 0.0f ? 0.0f : r1; out->rect[2] = r2 > 1.0f ? 1.0f : r2; out->rect[3] = r3 > 1.0f ? 1.0f : r3;
    if (!((out->rect[2] - out->rect[0]) * (out->rect[3] - out->rect[1]) >= VRFB_MIN_AREA)) return VRFB_NONE;
    /* mezcla: el sprite semitransparente solo mezcla los texels con el bit de máscara; los demás se pintan opacos. Se mide en la fuente. */
    int blended = semi;
    if (semi && vram) {
        int n = 0, m15 = 0;
        for (int y = sy0; y <= sy1; y += 4) for (int x = sx0; x <= sx1; x += 4) if (x >= 0 && x < 1024 && y >= 0 && y < 512) { n++; m15 += vram[y * 1024 + x] >> 15; }
        blended = n > 0 && 2 * m15 >= n;
    }
    static const float A[4] = {0.5f, 1.0f, 1.0f, 1.0f}, B[4] = {0.5f, 1.0f, -1.0f, 0.25f};
    out->a = blended ? A[abr & 3] : 0.0f;
    for (int c = 0; c < 3; c++) {
        float g = raw ? 1.0f : (float)col[c] / 128.0f;
        out->b[c] = (blended ? B[abr & 3] : 1.0f) * g;
    }
    out->mode = mode;
    return mode;
}
