#pragma once
/* vr_bodylive: el cuerpo del jugador (y sus manos propias) a la cadencia del renderer. Solo cabecera, sin estado global; la usa vrhost.c (mv_vrhost_body_piece).
 *
 * Por qué. El cuerpo va en la escena nativa (25 por segundo) en el mundo del motor, y el renderer corrige TODA la escena con vr_smooth (el mundo se desliza entre escenas): el
 * cuerpo se deslizaba con el mundo y saltaba un paso de escena con cada escena nueva (diente de sierra respecto a la cabeza y a las manos del usuario, ~10 cm corriendo).
 * Aquí se guarda, por escena publicada, la pose de cada pieza del jugador en el ESPACIO DEL USUARIO (U = to_vr o world, con la escala separada) y por fotograma de render se
 * devuelve la matriz de mundo que, tras el to_vr de la escena que se dibuja y la corrección de vr_smooth de ese fotograma, deja la pieza en la pose interpolada entre las de
 * las últimas escenas (con el mismo punto del camino `alpha` que vr_smooth): el cuerpo queda fijo respecto a la cabeza y solo cambia con su animación, de forma continua. */
#include <math.h>
#include <stdint.h>
#include <string.h>
#include "ns_types.h"
#include "ns_bridge.h"   /* ns_xf_lerp */
#include "ns_hand.h"     /* ns_key_is_hand */

#define VBL_MAX 96            /* piezas del jugador + manos propias por escena */
#define VBL_GAP_NS 400000000ull
typedef struct { uint64_t id; int n; float s; uint32_t key[VBL_MAX]; uint16_t k[VBL_MAX]; uint8_t live[VBL_MAX]; NsXf u[VBL_MAX]; } VblPose;   /* id = NsScene.t_ns; s = metros por unidad del motor; u = pose en el espacio del usuario / s; live = la pieza sigue al mando */
typedef struct { VblPose p[3]; int n; } VblRing;   /* p[0] la última escena, p[1] la anterior, p[2] la ante-anterior; n = cuántas valen */

/* Pose de las piezas del jugador (instancia map->key) y de sus manos propias en la escena `sc`. `live_parts` = partes del modelo (bit k = parte k, ns_player.h) que la IK lleva con
 * el mando: esas piezas y las manos propias se dibujan SIEMPRE en su última pose (la del mando más reciente; el arma y el espacio del usuario van en tiempo real), no una escena por
 * detrás como el resto del cuerpo. Devuelve cuántas piezas (0 = escena sin mapa VR, sin jugador o no caben: no vale). */
static inline int vbl_capture(VblPose *o, const NsScene *sc, const NsPlayerMap *map, uint32_t live_parts) {
    o->n = 0;
    if (!sc || !map || !(sc->valid & NS_SV_TOVR_OK)) return 0;
    const uint32_t player_key = map->key;
    const float *t = sc->to_vr;
    float s = sqrtf(t[0] * t[0] + t[4] * t[4] + t[8] * t[8]);
    if (!(s > 0.0f) || !isfinite(s)) return 0;
    int n = 0;
    for (uint32_t i = 0; i < sc->n_inst; i++) {
        const NsInstance *in = &sc->inst[i];
        if (in->key != player_key && !ns_key_is_hand(in->key)) continue;
        if ((uint64_t)in->first_piece + in->n_pieces > sc->n_piece) return 0;
        for (uint32_t k = 0; k < in->n_pieces; k++) {
            if (n >= VBL_MAX) return 0;
            const float *w = sc->piece[in->first_piece + k].world.m;
            float *u = o->u[n].m;
            for (int r = 0; r < 3; r++) {
                for (int c = 0; c < 4; c++) u[4 * r + c] = (t[4 * r] * w[c] + t[4 * r + 1] * w[4 + c] + t[4 * r + 2] * w[8 + c]) / s;
                u[4 * r + 3] += t[4 * r + 3] / s;
            }
            o->key[n] = in->key; o->k[n] = (uint16_t)k; o->live[n] = (uint8_t)ns_key_is_hand(in->key);
            for (int q = 0; q < NS_PLAYER_PARTS && !o->live[n] && in->key == player_key; q++)
                for (int e = 0; (live_parts >> q & 1u) && e < map->part[q].n; e++) if (map->part[q].idx[e] == in->first_piece + k) o->live[n] = 1;
            n++;
        }
    }
    o->id = sc->t_ns; o->s = s; o->n = n;
    return n;
}

/* Escena nueva publicada. `p` sin piezas (NULL o n = 0), un hueco largo o un salto de la primera pieza de más de 0,5 m (recentrado, cambio de zona): se empieza de cero. */
static inline void vbl_push(VblRing *r, const VblPose *p) {
    if (!p || p->n <= 0) { r->n = 0; return; }
    if (r->n) {
        const VblPose *q = &r->p[0];
        float d = 0.0f;
        for (int i = 0; i < 3; i++) { float e = p->u[0].m[4 * i + 3] * p->s - q->u[0].m[4 * i + 3] * q->s; d += e * e; }
        if (p->id - q->id > VBL_GAP_NS || q->key[0] != p->key[0] || !(d < 0.25f)) r->n = 0;
    }
    r->p[2] = r->p[1]; r->p[1] = r->p[0]; r->p[0] = *p;
    if (r->n < 3) r->n++;
}

static inline int vbl_find(const VblPose *p, uint32_t key, uint32_t k) {
    for (int i = 0; i < p->n; i++) if (p->key[i] == key && p->k[i] == k) return i;
    return -1;
}

/* Matriz de mundo (3x4 fila-mayor, como NsPiece.world) de la pieza `k` de la instancia `inst` de la escena `sc` QUE SE VA A DIBUJAR, para que tras sc->to_vr y la corrección `m`
 * (VrSmCorr.m, 4x4 columna-mayor, rígida) quede en la pose interpolada: alpha 0..1 de la escena anterior a la última, -1..0 de la ante-anterior a la anterior (VrSmCorr.alpha).
 * Las piezas que siguen al mando (VblPose.live) van en la última pose sea cual sea alpha.
 * Devuelve 0 (no tocar la pieza) si no hay dos poses, la escena no es ni la última ni la anterior, la instancia no es del jugador o la pieza no está en ambas poses. */
static inline int vbl_piece(const VblRing *r, const NsScene *sc, uint32_t inst, uint32_t k, float alpha, const float m[16], NsXf *out) {
    if (!r || r->n < 2 || !sc || !out || inst >= sc->n_inst || !isfinite(alpha)) return 0;
    if (sc->t_ns != r->p[0].id && sc->t_ns != r->p[1].id) return 0;
    const VblPose *a = &r->p[1], *b = &r->p[0];
    float t = alpha > 1.0f ? 1.0f : alpha;
    if (t < 0.0f) {
        if (r->n < 3) t = 0.0f;
        else { a = &r->p[2]; b = &r->p[1]; t = t < -1.0f ? 0.0f : t + 1.0f; }
    }
    uint32_t key = sc->inst[inst].key;
    int il = vbl_find(&r->p[0], key, k), ia = vbl_find(a, key, k), ib = vbl_find(b, key, k);
    if (il < 0 || ia < 0 || ib < 0) return 0;
    NsXf u;
    if (r->p[0].live[il]) u = r->p[0].u[il];
    else ns_xf_lerp(&a->u[ia], &b->u[ib], t, &u);
    /* x = inv(m) o (s u): m rígida (P' = Mr P + mt) */
    float x[12], s = r->p[0].s;
    for (int i = 0; i < 3; i++) {
        for (int c = 0; c < 3; c++) x[4 * i + c] = s * (m[4 * i] * u.m[c] + m[4 * i + 1] * u.m[4 + c] + m[4 * i + 2] * u.m[8 + c]);
        x[4 * i + 3] = m[4 * i] * (s * u.m[3] - m[12]) + m[4 * i + 1] * (s * u.m[7] - m[13]) + m[4 * i + 2] * (s * u.m[11] - m[14]);
    }
    /* out = inv(to_vr) o x */
    const float *v = sc->to_vr;
    float c00 = v[5] * v[10] - v[6] * v[9], c01 = v[6] * v[8] - v[4] * v[10], c02 = v[4] * v[9] - v[5] * v[8];
    float det = v[0] * c00 + v[1] * c01 + v[2] * c02;
    if (!(fabsf(det) > 1e-18f)) return 0;
    float id = 1.0f / det;
    float iv[9] = {c00 * id, (v[2] * v[9] - v[1] * v[10]) * id, (v[1] * v[6] - v[2] * v[5]) * id,
                   c01 * id, (v[0] * v[10] - v[2] * v[8]) * id, (v[2] * v[4] - v[0] * v[6]) * id,
                   c02 * id, (v[1] * v[8] - v[0] * v[9]) * id, (v[0] * v[5] - v[1] * v[4]) * id};
    x[3] -= v[3]; x[7] -= v[7]; x[11] -= v[11];
    for (int i = 0; i < 3; i++) for (int c = 0; c < 4; c++) {
        out->m[4 * i + c] = iv[3 * i] * x[c] + iv[3 * i + 1] * x[4 + c] + iv[3 * i + 2] * x[8 + c];
        if (!isfinite(out->m[4 * i + c])) return 0;
    }
    return 1;
}
