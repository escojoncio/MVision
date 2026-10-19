/* Malla de mano SINTÉTICA para las pruebas de vr_fist (no son datos del juego): manopla con muñeca, palma, bloque de dedos en tres anillos y pulgar suelto.
 * Convención de la mano derecha del personaje: palma +X, dedos -Y, pulgar +Z. mirror = 1 refleja X (palma -X: mano izquierda). */
#pragma once
#include <stdint.h>
#include <string.h>
#include <math.h>

typedef struct { int nv, nn, nf; int16_t v[128 * 4], n[128 * 4]; uint8_t vidx[128 * 4], nidx[128 * 4]; } SynthHand;

static void synth_hand(SynthHand *h, int mirror) {
    memset(h, 0, sizeof *h);
    static const float ring_y[5] = {20, -70, -100, -125, 0};
    static const float xlo[4] = {-20, -20, -8, 18}, xhi[4] = {30, 5, 10, 32}, zhw[4] = {40, 40, 36, 30};
    int nv = 0;
    for (int r = 0; r < 4; r++) {
        float xs[4] = {xlo[r], xlo[r], xhi[r], xhi[r]}, zs[4] = {-zhw[r], zhw[r], zhw[r], -zhw[r]};
        for (int k = 0; k < 4; k++) { h->v[4 * nv] = (int16_t)xs[k]; h->v[4 * nv + 1] = (int16_t)ring_y[r]; h->v[4 * nv + 2] = (int16_t)zs[k]; nv++; }
    }
    /* pulgar: caja aparte (8 vértices) cerca de la palma, hacia +Z */
    for (int ix = 0; ix < 2; ix++) for (int iy = 0; iy < 2; iy++) for (int iz = 0; iz < 2; iz++) {
        h->v[4 * nv] = (int16_t)(ix ? 55 : 25); h->v[4 * nv + 1] = (int16_t)(iy ? -62 : -10); h->v[4 * nv + 2] = (int16_t)(iz ? 45 : 10); nv++;
    }
    int nf = 0;
    for (int r = 0; r < 3; r++) for (int k = 0; k < 4; k++) {            /* paredes entre anillos */
        int a = 4 * r + k, b = 4 * r + ((k + 1) & 3), c = 4 * (r + 1) + ((k + 1) & 3), d = 4 * (r + 1) + k;
        h->vidx[4 * nf] = (uint8_t)a; h->vidx[4 * nf + 1] = (uint8_t)b; h->vidx[4 * nf + 2] = (uint8_t)c; h->vidx[4 * nf + 3] = (uint8_t)d; nf++;
    }
    h->vidx[4 * nf] = 12; h->vidx[4 * nf + 1] = 13; h->vidx[4 * nf + 2] = 14; h->vidx[4 * nf + 3] = 15; nf++;   /* punta */
    static const int box[6][4] = {{0, 1, 3, 2}, {4, 6, 7, 5}, {0, 4, 5, 1}, {2, 3, 7, 6}, {0, 2, 6, 4}, {1, 5, 7, 3}};
    for (int f = 0; f < 6; f++) { for (int k = 0; k < 4; k++) h->vidx[4 * nf + k] = (uint8_t)(16 + box[f][k]); nf++; }
    /* una normal por esquina de cara: la dirección desde el centroide de la malla hacia el vértice (unitaria, 4096 = 1) */
    int nn = 0;
    for (int f = 0; f < nf; f++) for (int k = 0; k < 4; k++) {
        int vi = h->vidx[4 * f + k];
        float cx = vi < 16 ? 5 : 40, cy = vi < 16 ? -50 : -36, cz = vi < 16 ? 0 : 27;
        float dx = h->v[4 * vi] - cx, dy = h->v[4 * vi + 1] - cy, dz = h->v[4 * vi + 2] - cz, l = sqrtf(dx * dx + dy * dy + dz * dz);
        if (l < 1e-3f) { dx = 1; dy = dz = 0; l = 1; }
        h->n[4 * nn] = (int16_t)lrintf(dx / l * 4096); h->n[4 * nn + 1] = (int16_t)lrintf(dy / l * 4096); h->n[4 * nn + 2] = (int16_t)lrintf(dz / l * 4096);
        h->nidx[4 * f + k] = (uint8_t)(nn | ((f & 1) ? 0x80 : 0)); nn++;                        /* bit 7 = marca: se ignora */
    }
    for (int f = 0; f < nf; f++) if (f % 3 == 0) for (int k = 0; k < 4; k++) h->vidx[4 * f + k] |= 0x80;   /* marcas de vértice: también se ignoran */
    if (mirror) {
        for (int i = 0; i < nv; i++) h->v[4 * i] = (int16_t)-h->v[4 * i];
        for (int i = 0; i < nn; i++) h->n[4 * i] = (int16_t)-h->n[4 * i];
    }
    h->nv = nv; h->nn = nn; h->nf = nf;
}
