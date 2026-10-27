/* vr_hand_asset: GENERADO por tools/hand/glb2c.py a partir de la mano genérica de WebXR Input Profiles (profiles/generic-hand/right.glb). NO EDITAR A MANO.
 *
 * Mano DERECHA en el marco de la pieza mano del jugador: mm, origen en la articulación de la muñeca, dedos -Y, palma +X, pulgar +Z (la izquierda es el espejo X).
 * Esqueleto OpenXR de 25 articulaciones (vrha_name; padres en vrha_parent). Ejes locales de cada articulación (convención OpenXR): -Z hacia la punta, +Y dorso, X = Y x Z
 * (eje de flexión). Malla con skin lineal: <= 4 huesos por vértice (vrha_joint, vrha_weight, suma 1), triángulos en sentido antihorario visto desde fuera (normales hacia
 * fuera; convención glTF, al revés que el motor), repartidos en piezas de <= 256 vértices (vrha_piece; índices locales a la pieza, vértices compartidos duplicados).
 * Cortada en la muñeca (y = -14.0 mm): sin el puño del asset; el borde abierto (vrha_cuff) lo cierra la transición al antebrazo del juego que genera ns_hand.
 * Original: 1360 vértices, 2186 triángulos (soldado); aquí 791 vértices (con duplicados entre piezas), 1400 triángulos, 4 piezas. Largo 175.7 mm (muñeca a yema del corazón 189.7 mm).
 *
 * Asset: profiles/generic-hand (right.glb) del paquete @webxr-input-profiles/assets 1.0.19 (repositorio immersive-web/webxr-input-profiles, commit f4992299, packages/assets).
 * Licencia del paquete (packages/assets/LICENSE.md):
 *
 * MIT License
 *
 * Copyright (c) 2019 Amazon
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is furnished
 * to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
 * FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS
 * OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF
 * OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VRHA_NJ 25
#define VRHA_NV 791
#define VRHA_NT 1400
#define VRHA_NP 4
#define VRHA_NCUFF 22
enum { VRHA_ROLE_PALM = 0, VRHA_ROLE_BACK = 1, VRHA_ROLE_SIDE = 2, VRHA_ROLE_TIP = 3 };   /* = NS_HT_PALM, NS_HT_BACK, NS_HT_FINGER, NS_HT_JOINT (ns_hand.h) */

typedef struct { uint16_t v0, nv, t0, nt; } VrHaPiece;

extern const float vrha_pos[VRHA_NV][3];       /* reposo, mm */
extern const float vrha_nrm[VRHA_NV][3];       /* unitaria hacia fuera */
extern const uint8_t vrha_joint[VRHA_NV][4];
extern const float vrha_weight[VRHA_NV][4];    /* suma 1 */
extern const uint8_t vrha_tri[VRHA_NT][3];     /* índices locales a su pieza */
extern const uint8_t vrha_role[VRHA_NT];       /* VRHA_ROLE_* */
extern const VrHaPiece vrha_piece[VRHA_NP];
extern const float vrha_bind[VRHA_NJ][12];     /* articulación -> marco de la mano en reposo (3x4 fila-mayor, mm) */
extern const float vrha_ibm[VRHA_NJ][12];      /* inversa de vrha_bind */
extern const int8_t vrha_parent[VRHA_NJ];
extern const char *const vrha_name[VRHA_NJ];
extern const uint16_t vrha_cuff[VRHA_NCUFF];   /* borde del corte de la muñeca (y = YCUT del generador), lazo ordenado: un vértice por punto (los duplicados en otras piezas tienen la misma posición) */

#ifdef __cplusplus
}
#endif
