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
/* Marco de la mano del mando en el espacio del usuario (m): p y cuaternión (x, y, z, w) de vrgame (agarre * desvío fijo, ver hand_frame_from_grip); ok = 0 sin seguimiento. */
typedef struct { int ok; float p[3], q[4]; } VblHand;
typedef struct { uint64_t id; int n; float s; uint32_t key[VBL_MAX]; uint16_t k[VBL_MAX]; uint8_t live[VBL_MAX]; uint8_t hs[VBL_MAX]; NsXf u[VBL_MAX]; VblHand hc[2]; } VblPose;   /* id = NsScene.t_ns; s = metros por unidad del motor; u = pose en el espacio del usuario / s; live = la pieza sigue al mando; hs = 1 + lado de la mano (0 izquierda, 1 derecha) cuyo marco mueve la pieza, 0 = ninguna; hc = marco de cada mano con el que se simuló la escena */
typedef struct { VblPose p[3]; int n; } VblRing;   /* p[0] la última escena, p[1] la anterior, p[2] la ante-anterior; n = cuántas valen */

/* Pose de las piezas del jugador (instancia map->key) y de sus manos propias en la escena `sc`. `live_parts` = partes del modelo (bit k = parte k, ns_player.h) que la IK lleva con
 * el mando: esas piezas y las manos propias se dibujan SIEMPRE en su última pose (la del mando más reciente; el arma y el espacio del usuario van en tiempo real), no una escena por
 * detrás como el resto del cuerpo. `held_key` (0 = ninguno) = instancia del objeto que el juego tiene en la mano (la granada: el arma nativa) y `held_side` el lado de la mano que lo sujeta; sus piezas y las de
 * la mano propia (las que van en el marco de la mano: la mano, no el anillo del antebrazo) se mueven en vbl_piece con el movimiento rígido del marco de esa mano, `hc` (NULL = ninguno), desde la escena hasta ahora.
 * Devuelve cuántas piezas (0 = escena sin mapa VR, sin jugador o no caben: no vale). */
static inline int vbl_capture(VblPose *o, const NsScene *sc, const NsPlayerMap *map, uint32_t live_parts, uint32_t held_key, int held_side, const VblHand hc[2]) {
    o->n = 0;
    if (!sc || !map || !(sc->valid & NS_SV_TOVR_OK)) return 0;
    const uint32_t player_key = map->key;
    const float *t = sc->to_vr;
    float s = sqrtf(t[0] * t[0] + t[4] * t[4] + t[8] * t[8]);
    if (!(s > 0.0f) || !isfinite(s)) return 0;
    int n = 0;
    for (uint32_t i = 0; i < sc->n_inst; i++) {
        const NsInstance *in = &sc->inst[i];
        const int held = held_key && in->key == held_key && in->key != player_key;
        if (in->key != player_key && !ns_key_is_hand(in->key) && !held) continue;
        if ((uint64_t)in->first_piece + in->n_pieces > sc->n_piece) return 0;
        const int hand_side = in->key == NS_HAND_KEY_R ? 1 : 0;
        const float *hw = in->n_pieces ? sc->piece[in->first_piece + in->n_pieces - 1].world.m : NULL;   /* marco de la mano: el de la última pieza de la mano propia */
        for (uint32_t k = 0; k < in->n_pieces; k++) {
            if (n >= VBL_MAX) return 0;
            const float *w = sc->piece[in->first_piece + k].world.m;
            float *u = o->u[n].m;
            for (int r = 0; r < 3; r++) {
                for (int c = 0; c < 4; c++) u[4 * r + c] = (t[4 * r] * w[c] + t[4 * r + 1] * w[4 + c] + t[4 * r + 2] * w[8 + c]) / s;
                u[4 * r + 3] += t[4 * r + 3] / s;
            }
            o->key[n] = in->key; o->k[n] = (uint16_t)k; o->live[n] = (uint8_t)(ns_key_is_hand(in->key) || held);
            o->hs[n] = held ? (uint8_t)(1 + (held_side ? 1 : 0)) : (uint8_t)(ns_key_is_hand(in->key) && !memcmp(w, hw, 12 * sizeof(float)) ? 1 + hand_side : 0);
            for (int q = 0; q < NS_PLAYER_PARTS && !o->live[n] && in->key == player_key; q++)
                for (int e = 0; (live_parts >> q & 1u) && e < map->part[q].n; e++) if (map->part[q].idx[e] == in->first_piece + k) o->live[n] = 1;
            n++;
        }
    }
    o->id = sc->t_ns; o->s = s; o->n = n;
    for (int i = 0; i < 2; i++) { if (hc) o->hc[i] = hc[i]; else memset(&o->hc[i], 0, sizeof o->hc[i]); }
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

/* Movimiento rígido del marco de la mano desde la escena (c) hasta ahora (n): D = Rn Rc^T, d = pn - D pc (m). Mismas cotas que el arma (mv_host_hands): un salto de más de 0,35 m o 1,2 rad
 * (recentrado, pérdida de seguimiento) no es movimiento de la mano y no se compensa. 0 = sin compensación. */
static inline int vbl_hand_delta(const VblHand *c, const VblHand *n, float D[9], float d[3]) {
    if (!c->ok || !n->ok) return 0;
    float qc[4], qn[4], q[4], nc = 0.0f, nn = 0.0f;
    for (int i = 0; i < 4; i++) { nc += c->q[i] * c->q[i]; nn += n->q[i] * n->q[i]; }
    if (!(nc > 1e-6f) || !(nn > 1e-6f) || !isfinite(nc) || !isfinite(nn)) return 0;
    nc = 1.0f / sqrtf(nc); nn = 1.0f / sqrtf(nn);
    for (int i = 0; i < 4; i++) { qc[i] = c->q[i] * nc * (i < 3 ? -1.0f : 1.0f); qn[i] = n->q[i] * nn; }   /* qc = conj(c) */
    q[0] = qn[3] * qc[0] + qn[0] * qc[3] + qn[1] * qc[2] - qn[2] * qc[1];
    q[1] = qn[3] * qc[1] - qn[0] * qc[2] + qn[1] * qc[3] + qn[2] * qc[0];
    q[2] = qn[3] * qc[2] + qn[0] * qc[1] - qn[1] * qc[0] + qn[2] * qc[3];
    q[3] = qn[3] * qc[3] - qn[0] * qc[0] - qn[1] * qc[1] - qn[2] * qc[2];
    float x = q[0], y = q[1], z = q[2], w = q[3];
    D[0] = 1 - 2 * (y * y + z * z); D[1] = 2 * (x * y - z * w);     D[2] = 2 * (x * z + y * w);
    D[3] = 2 * (x * y + z * w);     D[4] = 1 - 2 * (x * x + z * z); D[5] = 2 * (y * z - x * w);
    D[6] = 2 * (x * z - y * w);     D[7] = 2 * (y * z + x * w);     D[8] = 1 - 2 * (x * x + y * y);
    float move = 0.0f;
    for (int i = 0; i < 3; i++) {
        d[i] = n->p[i] - (D[3 * i] * c->p[0] + D[3 * i + 1] * c->p[1] + D[3 * i + 2] * c->p[2]);
        move += (n->p[i] - c->p[i]) * (n->p[i] - c->p[i]);
    }
    return isfinite(move) && isfinite(d[0] + d[1] + d[2]) && move < 0.35f * 0.35f && 2.0f * acosf(fminf(1.0f, fabsf(w))) < 1.2f;
}

/* Matriz de mundo (3x4 fila-mayor, como NsPiece.world) de la pieza `k` de la instancia `inst` de la escena `sc` QUE SE VA A DIBUJAR, para que tras sc->to_vr y la corrección `m`
 * (VrSmCorr.m, 4x4 columna-mayor, rígida; NULL = el renderer no corrige este fotograma) quede en la pose interpolada: alpha 0..1 de la escena anterior a la última, -1..0 de la ante-anterior a la anterior (VrSmCorr.alpha).
 * Las piezas que siguen al mando (VblPose.live) van en la última pose sea cual sea alpha, la escena (la que sea del anillo), el estado de la fluidez (m NULL = sin corrección) o el número de poses
 * (basta una), y si son de una mano (VblPose.hs) con el movimiento rígido de su marco desde la escena hasta `now` (marco de ahora de cada mano; NULL = sin compensar): la misma compensación que el arma.
 * Devuelve 0 (no tocar la pieza) si la escena no es del anillo, la instancia no es del jugador, la pieza no está en la última pose o, si no sigue al mando, no hay dos poses, la escena no es ni la última ni la
 * anterior, no hay corrección (m NULL) o la pieza no está en ambas poses. */
static inline int vbl_piece(const VblRing *r, const NsScene *sc, uint32_t inst, uint32_t k, float alpha, const float m[16], const VblHand now[2], NsXf *out) {
    static const float I4[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    if (!r || r->n < 1 || !sc || !out || inst >= sc->n_inst || !isfinite(alpha)) return 0;
    int known = 0;
    for (int i = 0; i < r->n; i++) known |= sc->t_ns == r->p[i].id;
    if (!known) return 0;
    uint32_t key = sc->inst[inst].key;
    int il = vbl_find(&r->p[0], key, k);
    if (il < 0) return 0;
    NsXf u;
    if (r->p[0].live[il]) {
        u = r->p[0].u[il];
        float D[9], d[3];
        if (r->p[0].hs[il] && now && vbl_hand_delta(&r->p[0].hc[r->p[0].hs[il] - 1], &now[r->p[0].hs[il] - 1], D, d)) {
            NsXf v;
            for (int i = 0; i < 3; i++) {
                for (int c = 0; c < 3; c++) v.m[4 * i + c] = D[3 * i] * u.m[c] + D[3 * i + 1] * u.m[4 + c] + D[3 * i + 2] * u.m[8 + c];
                v.m[4 * i + 3] = D[3 * i] * u.m[3] + D[3 * i + 1] * u.m[7] + D[3 * i + 2] * u.m[11] + d[i] / r->p[0].s;
            }
            u = v;
        }
    } else {
        if (!m || r->n < 2 || (sc->t_ns != r->p[0].id && sc->t_ns != r->p[1].id)) return 0;
        const VblPose *a = &r->p[1], *b = &r->p[0];
        float t = alpha > 1.0f ? 1.0f : alpha;
        if (t < 0.0f) {
            if (r->n < 3) t = 0.0f;
            else { a = &r->p[2]; b = &r->p[1]; t = t < -1.0f ? 0.0f : t + 1.0f; }
        }
        int ia = vbl_find(a, key, k), ib = vbl_find(b, key, k);
        if (ia < 0 || ib < 0) return 0;
        ns_xf_lerp(&a->u[ia], &b->u[ib], t, &u);
    }
    if (!m) m = I4;
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
