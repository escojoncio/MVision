#!/usr/bin/env python3
"""glb2c: convierte la mano genérica de WebXR Input Profiles (profiles/generic-hand/right.glb, esqueleto OpenXR de 25 articulaciones, licencia MIT, ver ASSET_LICENSE.md)
en vr/body/vr_hand_asset.c/.h. Solo numpy (lector GLB propio). Ejecutar con `python3 -I`:

  python3 -I tools/hand/glb2c.py <right.glb> <dir_salida> [--tris 1400]

Qué hace
  - Marco de la mano del motor (el de la pieza mano del jugador): mm, origen en la articulación de la muñeca, dedos -Y, palma +X (mano DERECHA), pulgar +Z. El asset (metros,
    dedos -Y, palma -X, pulgar -Z) se gira 180 grados sobre Y y se traslada a la muñeca. La mano izquierda NO se exporta: es el espejo X de la derecha (como el resto del código).
  - Suelda los vértices duplicados por las costuras de UV (no se usan UV) y diezma con colapso de media arista por cuádricas (QEM) hasta --tris triángulos, conservando el borde
    de la muñeca, la orientación de las caras y, con penalización, las fronteras entre pesos de huesos distintos.
  - Reparte los triángulos en piezas de <= 256 vértices (NsFace usa índices de 8 bits): los vértices compartidos entre piezas se duplican (mismo skin, sin costuras).
  - Rol de textura por triángulo (palma/yemas, dorso, costados, punta) con la normal y el hueso dominante en reposo.
Salida determinista (mismo .glb -> mismo .c)."""
import heapq
import json
import os
import struct
import sys

import numpy as np

NAMES = ["wrist",
         "thumb-metacarpal", "thumb-phalanx-proximal", "thumb-phalanx-distal", "thumb-tip",
         "index-finger-metacarpal", "index-finger-phalanx-proximal", "index-finger-phalanx-intermediate", "index-finger-phalanx-distal", "index-finger-tip",
         "middle-finger-metacarpal", "middle-finger-phalanx-proximal", "middle-finger-phalanx-intermediate", "middle-finger-phalanx-distal", "middle-finger-tip",
         "ring-finger-metacarpal", "ring-finger-phalanx-proximal", "ring-finger-phalanx-intermediate", "ring-finger-phalanx-distal", "ring-finger-tip",
         "pinky-finger-metacarpal", "pinky-finger-phalanx-proximal", "pinky-finger-phalanx-intermediate", "pinky-finger-phalanx-distal", "pinky-finger-tip"]
PARENT = [-1, 0, 1, 2, 3, 0, 5, 6, 7, 8, 0, 10, 11, 12, 13, 0, 15, 16, 17, 18, 0, 20, 21, 22, 23]
CT = {5126: np.float32, 5121: np.uint8, 5123: np.uint16, 5125: np.uint32}
NC = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4, "MAT4": 16}
ROLE_PALM, ROLE_BACK, ROLE_SIDE, ROLE_TIP = 0, 1, 2, 3     # = NS_HT_PALM, NS_HT_BACK, NS_HT_FINGER, NS_HT_JOINT de ns_hand.h
YCUT = -14.0     # corte de la muñeca (mm, marco de la mano): se quita el puño del asset; la transición al antebrazo del juego la genera ns_hand


def load_glb(path):
    d = open(path, "rb").read()
    if d[:4] != b"glTF":
        raise SystemExit("no es un GLB")
    off, js, bn = 12, None, None
    while off < len(d):
        ln, ty = struct.unpack_from("<II", d, off)
        off += 8
        c = d[off:off + ln]
        off += ln
        if ty == 0x4E4F534A:
            js = json.loads(c)
        elif ty == 0x004E4942:
            bn = c
    return js, bn


def accessor(js, bn, i):
    a = js["accessors"][i]
    bv = js["bufferViews"][a["bufferView"]]
    dt, n = CT[a["componentType"]], NC[a["type"]]
    o = bv.get("byteOffset", 0) + a.get("byteOffset", 0)
    isz = np.dtype(dt).itemsize * n
    st = bv.get("byteStride", 0) or isz
    raw = np.frombuffer(bn, np.uint8, count=st * (a["count"] - 1) + isz, offset=o)
    out = np.empty((a["count"], n), dt)
    for k in range(a["count"]):
        out[k] = np.frombuffer(raw[k * st:k * st + isz].tobytes(), dt)
    return out


def quat_mat(q):
    x, y, z, w = q
    return np.array([[1 - 2 * (y * y + z * z), 2 * (x * y - w * z), 2 * (x * z + w * y)],
                     [2 * (x * y + w * z), 1 - 2 * (x * x + z * z), 2 * (y * z - w * x)],
                     [2 * (x * z - w * y), 2 * (y * z + w * x), 1 - 2 * (x * x + y * y)]])


def node_mat(n):
    if "matrix" in n:
        return np.array(n["matrix"], float).reshape(4, 4).T
    M = np.eye(4)
    M[:3, :3] = quat_mat(n.get("rotation", [0, 0, 0, 1])) @ np.diag(n.get("scale", [1, 1, 1]))
    M[:3, 3] = n.get("translation", [0, 0, 0])
    return M


def tri_normal(P, t):
    n = np.cross(P[t[1]] - P[t[0]], P[t[2]] - P[t[0]])
    l = np.linalg.norm(n)
    return n / l if l > 1e-12 else n, 0.5 * l


# ------------------------------------------------------------------------------------------------------------- diezmado (QEM, colapso de media arista)
def decimate(P, T, Wfull, target, wpen):
    nv = len(P)
    tris = [list(t) for t in T]
    alive = [True] * len(tris)
    vt = [set() for _ in range(nv)]
    for i, t in enumerate(tris):
        for v in t:
            vt[v].add(i)
    Q = np.zeros((nv, 4, 4))
    for t in tris:
        n, a = tri_normal(P, t)
        p = np.append(n, -n.dot(P[t[0]]))
        Q[list(t)] += a * np.outer(p, p)

    def edges_of(v):
        s = set()
        for ti in vt[v]:
            for u in tris[ti]:
                if u != v:
                    s.add(u)
        return s

    def edge_tris(a, b):
        return [ti for ti in vt[a] if b in tris[ti]]

    def is_boundary_edge(a, b):
        return len(edge_tris(a, b)) == 1

    bnd = [False] * nv
    for a in range(nv):
        for b in edges_of(a):
            if is_boundary_edge(a, b):
                bnd[a] = True
                n, ar = tri_normal(P, tris[edge_tris(a, b)[0]])
                e = P[b] - P[a]
                m = np.cross(e, n)
                l = np.linalg.norm(m)
                if l > 1e-12:
                    m /= l
                    p = np.append(m, -m.dot(P[a]))
                    Q[a] += 1e3 * np.outer(p, p)    # el borde de la muñeca no se mueve
    ver = [0] * nv

    def cost(a, b):          # quitar a, conservar b
        if bnd[a] and not (bnd[b] and is_boundary_edge(a, b)):
            return None
        if bnd[a] and bnd[b] and not is_boundary_edge(a, b):
            return None
        q = Q[a] + Q[b]
        x = np.append(P[b], 1.0)
        return float(x @ q @ x) + wpen * float(np.abs(Wfull[a] - Wfull[b]).sum())

    def valid(a, b):
        na, nb = edges_of(a), edges_of(b)
        common = na & nb
        if len(common) != len(edge_tris(a, b)):           # condición de enlace (variedad)
            return False
        for ti in vt[a]:
            t = tris[ti]
            if b in t:
                continue
            n0, _ = tri_normal(P, t)
            t2 = [b if v == a else v for v in t]
            n1, ar = tri_normal(P, t2)
            if ar < 1e-4 or n0.dot(n1) < 0.3:              # sin caras invertidas ni degeneradas
                return False
        return True

    heap = []

    def push(a):
        for b in edges_of(a):
            for x, y in ((a, b), (b, a)):
                c = cost(x, y)
                if c is not None:
                    heapq.heappush(heap, (c, x, y, ver[x], ver[y]))
    for a in range(nv):
        push(a)
    ntri = len(tris)
    removed = [False] * nv
    while ntri > target and heap:
        c, a, b, va, vb = heapq.heappop(heap)
        if removed[a] or removed[b] or va != ver[a] or vb != ver[b]:
            continue
        if b not in edges_of(a) or not valid(a, b):
            continue
        for ti in list(vt[a]):
            t = tris[ti]
            if b in t:
                alive[ti] = False
                ntri -= 1
                for v in t:
                    vt[v].discard(ti)
            else:
                tris[ti] = [b if v == a else v for v in t]
                vt[b].add(ti)
        vt[a] = set()
        removed[a] = True
        Q[b] += Q[a]
        nb = edges_of(b)
        for v in nb | {b}:
            ver[v] += 1
        for v in nb | {b}:
            push(v)
    keep = [i for i in range(nv) if not removed[i] and vt[i]]
    remap = {v: i for i, v in enumerate(keep)}
    T2 = [[remap[v] for v in tris[i]] for i in range(len(tris)) if alive[i]]
    return np.array(keep), np.array(T2, int)



# ------------------------------------------------------------------------------------------------------------- corte en la muñeca
def clip_below(P, N, W, T, yc):
    """Se queda con la parte de la malla por debajo del plano y = yc (hacia los dedos): quita el puño del asset más allá de la muñeca. Los triángulos que cruzan se recortan
    con vértices nuevos exactamente en el plano (posición, normal y pesos interpolados), conservando el sentido. Devuelve P, N, W, T nuevos."""
    P, N, W = [p_ for p_ in P], [n_ for n_ in N], [w_ for w_ in W]
    cache = {}

    def cut(a, b):
        k_ = (min(a, b), max(a, b))
        if k_ not in cache:
            t = (yc - P[a][1]) / (P[b][1] - P[a][1])
            P.append(P[a] + t * (P[b] - P[a]))
            n_ = N[a] + t * (N[b] - N[a])
            N.append(n_ / np.linalg.norm(n_))
            w_ = W[a] + t * (W[b] - W[a])
            W.append(w_ / w_.sum())
            cache[k_] = len(P) - 1
        return cache[k_]
    out = []
    for t in T.tolist():
        inside = [P[v][1] <= yc for v in t]
        if all(inside):
            out.append(t)
            continue
        if not any(inside):
            continue
        poly = []
        for i in range(3):
            a, b = t[i], t[(i + 1) % 3]
            if inside[i]:
                poly.append(a)
            if inside[i] != inside[(i + 1) % 3]:
                poly.append(cut(a, b))
        for i in range(1, len(poly) - 1):
            out.append([poly[0], poly[i], poly[i + 1]])
    return np.array(P), np.array(N), np.array(W), np.array(out, int)


def boundary_loops(T):
    """Bordes abiertos de la malla como lazos ordenados (siguiendo el sentido de los triángulos)."""
    cnt, nxt = {}, {}
    for t in T.tolist():
        for a_, b_ in ((t[0], t[1]), (t[1], t[2]), (t[2], t[0])):
            k_ = (min(a_, b_), max(a_, b_))
            cnt[k_] = cnt.get(k_, 0) + 1
    for t in T.tolist():
        for a_, b_ in ((t[0], t[1]), (t[1], t[2]), (t[2], t[0])):
            if cnt[(min(a_, b_), max(a_, b_))] == 1:
                nxt[b_] = a_                     # el borde se recorre al revés que el triángulo
    loops, seen = [], set()
    for v0 in nxt:
        if v0 in seen:
            continue
        loop, v = [], v0
        while v not in seen:
            seen.add(v)
            loop.append(v)
            v = nxt[v]
        loops.append(loop)
    return loops

# ------------------------------------------------------------------------------------------------------------- reparto en piezas de <= 256 vértices
def split_pieces(T, dom, P, maxv=256):
    group = np.array([0 if j == 0 else (1 if j <= 4 else 2 + (j - 5) // 5) for j in range(25)])
    # 0 muñeca/palma, 1 pulgar, 2 índice, 3 corazón, 4 anular, 5 meñique
    order = sorted(range(len(T)), key=lambda i: (group[dom[i]], round(float(P[T[i]].mean(0)[1]), 1), i))
    pieces, cur, cv = [], [], set()
    for i in order:
        nvs = cv | set(T[i].tolist())
        if len(nvs) > maxv:
            pieces.append(cur)
            cur, cv = [], set()
            nvs = set(T[i].tolist())
        cur.append(i)
        cv = nvs
    if cur:
        pieces.append(cur)
    return pieces


def fmt_floats(vals, prec=4):
    return ", ".join(("%.*ff" % (prec, v)).replace("-0.0000f", "0.0000f") for v in vals)


def main():
    a = sys.argv[1:]
    if len(a) < 2:
        raise SystemExit(__doc__)
    target = 1400
    if "--tris" in a:
        target = int(a[a.index("--tris") + 1])
    js, bn = load_glb(a[0])
    skin = js["skins"][0]
    jnodes = skin["joints"]
    names = [js["nodes"][j]["name"] for j in jnodes]
    if names != NAMES:
        raise SystemExit("orden de articulaciones inesperado: %s" % names)
    prim = js["meshes"][0]["primitives"][0]
    at = prim["attributes"]
    P = accessor(js, bn, at["POSITION"]).astype(float)
    N = accessor(js, bn, at["NORMAL"]).astype(float)
    J = accessor(js, bn, at["JOINTS_0"]).astype(int)
    W = accessor(js, bn, at["WEIGHTS_0"]).astype(float)
    T = accessor(js, bn, prim["indices"]).astype(int).reshape(-1, 3)
    G = [node_mat(js["nodes"][j]) for j in jnodes]        # jerarquía plana bajo Armature (sin transformación): globales = locales
    IBM = accessor(js, bn, skin["inverseBindMatrices"]).astype(float)
    for j in range(25):
        if np.abs(G[j] @ IBM[j].reshape(4, 4).T - np.eye(4)).max() > 1e-4:
            raise SystemExit("matriz inversa de reposo incoherente en %s" % NAMES[j])
    # al marco de la mano: giro de 180 grados sobre Y, origen en la muñeca, mm
    A = np.diag([-1.0, 1.0, -1.0])
    w0 = G[0][:3, 3].copy()
    P = (P - w0) @ A.T * 1000.0
    N = N @ A.T
    Gh = []
    for j in range(25):
        M = np.eye(4)
        M[:3, :3] = A @ G[j][:3, :3]
        M[:3, 3] = A @ (G[j][:3, 3] - w0) * 1000.0
        M[:3, :3] /= np.cbrt(np.linalg.det(M[:3, :3]))      # quita la escala residual (1 +- 1e-7)
        u, _, vt_ = np.linalg.svd(M[:3, :3])
        M[:3, :3] = u @ vt_
        Gh.append(M)
    # pesos densos (25 huesos) normalizados
    Wfull = np.zeros((len(P), 25))
    for i in range(len(P)):
        for k in range(4):
            Wfull[i, J[i, k]] += W[i, k]
    Wfull /= Wfull.sum(1, keepdims=True)
    # soldar duplicados por posición (costuras de UV)
    key = {}
    weld = np.zeros(len(P), int)
    for i in range(len(P)):
        k = tuple(np.round(P[i], 4))
        if k not in key:
            key[k] = i
        weld[i] = key[k]
    uniq = sorted(set(weld.tolist()))
    um = {v: i for i, v in enumerate(uniq)}
    Nw = np.zeros((len(uniq), 3))
    for i in range(len(P)):
        Nw[um[weld[i]]] += N[i]
    Nw /= np.linalg.norm(Nw, axis=1, keepdims=True)
    P1, W1 = P[uniq], Wfull[uniq]
    T1 = np.array([[um[weld[v]] for v in t] for t in T])
    T1 = T1[[len(set(t)) == 3 for t in T1.tolist()]]
    P1, Nw, W1, T1 = clip_below(P1, Nw, W1, T1, YCUT)
    if len(boundary_loops(T1)) != 1:
        raise SystemExit("el corte de la muñeca no deja un único borde")
    n0 = len(T1)
    keep, T2 = decimate(P1, T1, W1, target, wpen=4.0)
    P2, N2, W2 = P1[keep], Nw[keep], W1[keep]
    # 4 influencias como mucho por vértice, renormalizadas
    J4 = np.zeros((len(P2), 4), int)
    W4 = np.zeros((len(P2), 4))
    for i in range(len(P2)):
        o = np.argsort(-W2[i], kind="stable")[:4]
        w = W2[i, o]
        w = np.where(w > 1e-4, w, 0.0)
        w /= w.sum()
        J4[i], W4[i] = o, w
    # rol y hueso dominante por triángulo (reposo)
    dom, role = [], []
    tipj = {4, 9, 14, 19, 24, 3, 8, 13, 18, 23}
    for t in T2:
        s = W2[t].sum(0)
        d = int(np.argmax(s))
        dom.append(d)
        n, _ = tri_normal(P2, t)
        if d in tipj and n[1] < -0.65:
            role.append(ROLE_TIP)          # punta de los dedos
        elif n[0] > 0.35:
            role.append(ROLE_PALM)         # palma y yemas (cara palmar)
        elif n[0] < -0.35:
            role.append(ROLE_BACK)         # dorso
        else:
            role.append(ROLE_SIDE)         # costados de dedos y palma
    pieces = split_pieces(T2, dom, P2)
    # vértices por pieza (duplicados entre piezas), índices locales
    out_v, out_t, out_r, out_p = [], [], [], []
    for pc in pieces:
        loc = {}
        v0, t0 = len(out_v), len(out_t)
        for ti in pc:
            tt = []
            for v in T2[ti]:
                if v not in loc:
                    loc[v] = len(loc)
                    out_v.append(v)
                tt.append(loc[v])
            out_t.append(tt)
            out_r.append(role[ti])
        out_p.append((v0, len(out_v) - v0, t0, len(out_t) - t0))
    assert all(p[1] <= 256 for p in out_p)
    # borde del corte de la muñeca, en orden: un vértice (cualquiera de sus duplicados por pieza) por punto del lazo
    loops = boundary_loops(T2)
    if len(loops) != 1:
        raise SystemExit("tras el diezmado el borde de la muñeca no es un único lazo")
    first = {}
    for i, v in enumerate(out_v):
        first.setdefault(v, i)
    cuff = [first[v] for v in loops[0]]
    length = P2[:, 1].max() - P2[:, 1].min()
    tip_len = -P2[:, 1].min()
    write(a[1], out_v, out_t, out_r, out_p, P2, N2, J4, W4, Gh, n0, len(P), length, tip_len, cuff)


def write(outdir, out_v, out_t, out_r, out_p, P, N, J4, W4, Gh, n0, nraw, length, tip_len, cuff):
    nv, nt, npc = len(out_v), len(out_t), len(out_p)
    lic = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "ASSET_LICENSE.md")).read().strip()
    hdr = f"""/* vr_hand_asset: GENERADO por tools/hand/glb2c.py a partir de la mano genérica de WebXR Input Profiles (profiles/generic-hand/right.glb). NO EDITAR A MANO.
 *
 * Mano DERECHA en el marco de la pieza mano del jugador: mm, origen en la articulación de la muñeca, dedos -Y, palma +X, pulgar +Z (la izquierda es el espejo X).
 * Esqueleto OpenXR de 25 articulaciones (vrha_name; padres en vrha_parent). Ejes locales de cada articulación (convención OpenXR): -Z hacia la punta, +Y dorso, X = Y x Z
 * (eje de flexión). Malla con skin lineal: <= 4 huesos por vértice (vrha_joint, vrha_weight, suma 1), triángulos en sentido antihorario visto desde fuera (normales hacia
 * fuera; convención glTF, al revés que el motor), repartidos en piezas de <= 256 vértices (vrha_piece; índices locales a la pieza, vértices compartidos duplicados).
 * Cortada en la muñeca (y = {YCUT} mm): sin el puño del asset; el borde abierto (vrha_cuff) lo cierra la transición al antebrazo del juego que genera ns_hand.
 * Original: {nraw} vértices, {n0} triángulos (soldado); aquí {nv} vértices (con duplicados entre piezas), {nt} triángulos, {npc} piezas. Largo {length:.1f} mm (muñeca a yema del corazón {tip_len:.1f} mm).
 *
{chr(10).join(' * ' + l if l else ' *' for l in lic.splitlines())}
 */"""
    h = hdr + f"""
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {{
#endif

#define VRHA_NJ {25}
#define VRHA_NV {nv}
#define VRHA_NT {nt}
#define VRHA_NP {npc}
#define VRHA_NCUFF {len(cuff)}
enum {{ VRHA_ROLE_PALM = 0, VRHA_ROLE_BACK = 1, VRHA_ROLE_SIDE = 2, VRHA_ROLE_TIP = 3 }};   /* = NS_HT_PALM, NS_HT_BACK, NS_HT_FINGER, NS_HT_JOINT (ns_hand.h) */

typedef struct {{ uint16_t v0, nv, t0, nt; }} VrHaPiece;

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
}}
#endif
"""
    c = [hdr.replace("vr_hand_asset: GENERADO", "vr_hand_asset (datos): GENERADO", 1), '#include "vr_hand_asset.h"', ""]
    c.append("const float vrha_pos[VRHA_NV][3] = {")
    c += ["    {%s}," % fmt_floats(P[v], 3) for v in out_v]
    c.append("};")
    c.append("const float vrha_nrm[VRHA_NV][3] = {")
    c += ["    {%s}," % fmt_floats(N[v], 4) for v in out_v]
    c.append("};")
    c.append("const uint8_t vrha_joint[VRHA_NV][4] = {")
    c += ["    {%s}," % ", ".join(str(int(x)) for x in J4[v]) for v in out_v]
    c.append("};")
    c.append("const float vrha_weight[VRHA_NV][4] = {")
    for v in out_v:
        w = np.round(W4[v], 5)
        w[np.argmax(w)] += 1.0 - w.sum()
        c.append("    {%s}," % fmt_floats(w, 5))
    c.append("};")
    c.append("const uint8_t vrha_tri[VRHA_NT][3] = {")
    for i in range(0, nt, 8):
        c.append("    " + " ".join("{%d, %d, %d}," % tuple(t) for t in out_t[i:i + 8]))
    c.append("};")
    c.append("const uint8_t vrha_role[VRHA_NT] = {")
    for i in range(0, nt, 40):
        c.append("    " + " ".join("%d," % r for r in out_r[i:i + 40]))
    c.append("};")
    c.append("const VrHaPiece vrha_piece[VRHA_NP] = {")
    c += ["    {%d, %d, %d, %d}," % p for p in out_p]
    c.append("};")
    c.append("const float vrha_bind[VRHA_NJ][12] = {")
    for M in Gh:
        c.append("    {%s}," % fmt_floats(M[:3, :].reshape(-1), 6))
    c.append("};")
    c.append("const float vrha_ibm[VRHA_NJ][12] = {")
    for M in Gh:
        I = np.linalg.inv(M)
        c.append("    {%s}," % fmt_floats(I[:3, :].reshape(-1), 6))
    c.append("};")
    c.append("const int8_t vrha_parent[VRHA_NJ] = {%s};" % ", ".join(str(p) for p in PARENT))
    c.append("const char *const vrha_name[VRHA_NJ] = {")
    for i in range(0, 25, 5):
        c.append("    " + " ".join('"%s",' % n for n in NAMES[i:i + 5]))
    c.append("};")
    c.append("const uint16_t vrha_cuff[VRHA_NCUFF] = {")
    for i in range(0, len(cuff), 16):
        c.append("    " + " ".join("%d," % v for v in cuff[i:i + 16]))
    c.append("};")
    open(os.path.join(outdir, "vr_hand_asset.h"), "w").write(h)
    open(os.path.join(outdir, "vr_hand_asset.c"), "w").write("\n".join(c) + "\n")
    print("vr_hand_asset: %d vértices, %d triángulos, %d piezas %s, largo %.1f mm" % (nv, nt, npc, [p[1] for p in out_p], length))


if __name__ == "__main__":
    main()
