"""mdlfmt: lectura del formato de modelos y texturas del motor (solo biblioteca, stdlib).

Contenedor: ver tools/stage/stages.py. Ficheros del grupo 'c' (cache): cada tag da el
desplazamiento de su fichero dentro del grupo; el tag con ext 0xFF marca el final. Los tags de
modo 'n'/'r' llevan una secuencia de bloques {u32 id, u32 tamaño, datos}.
  id = (tipo << 16) | código de 16 bits derivado del nombre (ver id16).

Modelo (ext 'k'):
  cabecera 0x20: i32 nobj_visibles, i32 nobj_total, bbox min[3], bbox max[3]
  objeto (0x58 B, desde 0x20): u32 flags, u32 ncaras, bbox min/max (6 x i32), pos[3] i32 (relativa al padre),
     i32 padre (-1 = raíz), i32 -1, u32 nvert, off vert (int16 x,y,z,pad), off idx vértice (4 u8 por cara),
     u32 nnorm, off norm (int16 x,y,z,pad), off idx normal (4 u8 por cara, bit 7 = marca), off uv (4 x (u8,u8) por cara),
     off tex (u16 por cara = id16 de la textura), 0.
  Las caras son anillos de 4 vértices (triángulo = 4º índice repetido): tris (0,1,2) y (0,2,3).
Textura: imagen RLE estándar (8 bpp con paleta al final, o 4 bits/16 colores).
"""
import struct


def secs(n):
    return (n + 2047) // 2048


def id16(s):
    h = 0
    for ch in s.encode():
        h = (ch + (((h << 5) | (h >> 11)) & 0xFFFF)) & 0xFFFF
    return h


def stage_blocks(d, tb, stages_mod, name):
    """Devuelve (cache, bloques): cache = [(id16, ext, bytes)], bloques = [(id24, ext, bytes)] de tags n/r."""
    st = stages_mod.stage(d, tb[name])
    tags = st["tags"]
    cache, blocks = [], []
    cs = [t for t in tags if t["mode"] == "c"]
    if cs:
        base = cs[0]["sector"] * 2048
        for i in range(len(cs) - 1):
            a, b = cs[i]["size"], cs[i + 1]["size"]
            if cs[i]["ext"] != 0xFF:
                cache.append((cs[i]["id"], chr(cs[i]["ext"]), d[base + a:base + b]))
    for t in tags:
        if t["mode"] in "nr":
            b = d[t["sector"] * 2048:t["sector"] * 2048 + t["size"]]
            p = 0
            while p + 8 <= len(b):
                bid, sz = struct.unpack_from("<II", b, p)
                if sz == 0 or p + 8 + sz > len(b):
                    break
                blocks.append((bid, chr((bid >> 16) & 0xFF), b[p + 8:p + 8 + sz]))
                p += 8 + sz
    return cache, blocks


def parse_mdl(b):
    """-> lista de objetos con vértices, normales y caras; None si no es un modelo válido."""
    if len(b) < 0x20:
        return None
    nvis, n = struct.unpack_from("<ii", b, 0)   # visibles, total de objetos
    if not (0 < n <= 512) or nvis > n or 0x20 + n * 0x58 > len(b):
        return None
    bbox = struct.unpack_from("<6i", b, 8)
    objs = []
    for i in range(n):
        o = 0x20 + i * 0x58
        flags, nf = struct.unpack_from("<II", b, o)
        pos = struct.unpack_from("<3i", b, o + 0x20)
        parent = struct.unpack_from("<i", b, o + 0x2C)[0]
        nv, vo, io, nn, no, nio, uvo, to = struct.unpack_from("<8I", b, o + 0x34)
        if nf > 4000 or nv > 4000 or nn > 4000 or parent >= n:
            return None
        if max(vo + nv * 8, io + nf * 4, no + nn * 8, nio + nf * 4, uvo + nf * 8, to + nf * 2) > len(b):
            return None
        verts = [struct.unpack_from("<3h", b, vo + 8 * k) for k in range(nv)]
        norms = [struct.unpack_from("<3h", b, no + 8 * k) for k in range(nn)]
        faces = []
        for f in range(nf):
            vi = list(b[io + 4 * f:io + 4 * f + 4])
            ni = [x & 0x7F for x in b[nio + 4 * f:nio + 4 * f + 4]]
            uv = [(b[uvo + 8 * f + 2 * k], b[uvo + 8 * f + 2 * k + 1]) for k in range(4)]
            tex = struct.unpack_from("<H", b, to + 2 * f)[0]
            if max(vi) >= max(nv, 1):
                return None
            faces.append((vi, ni, uv, tex))
        objs.append({"flags": flags, "pos": pos, "parent": parent, "bbox": struct.unpack_from("<6i", b, o + 8),
                     "verts": verts, "norms": norms, "faces": faces})
    return {"bbox": bbox, "objs": objs}


def tri_list(face):
    """Cara de 4 esquinas en anillo -> triángulos (0,1,2),(0,2,3); el triángulo repite el 4º índice."""
    vi = face[0]
    t = [(0, 1, 2)]
    if vi[3] != vi[2] and vi[3] != vi[0]:
        t.append((0, 2, 3))
    return t


def world_offsets(m):
    """Posición acumulada de cada objeto (suma de pos de ancestros, sin rotación)."""
    out = []
    for i, o in enumerate(m["objs"]):
        p = list(o["pos"])
        q = o["parent"]
        g = 0
        while q >= 0 and g < 64:
            pp = m["objs"][q]["pos"]
            p = [p[k] + pp[k] for k in range(3)]
            q = m["objs"][q]["parent"]
            g += 1
        out.append(tuple(p))
    return out


def decode_img(b):
    """-> (ancho, alto, [(r,g,b)] paleta, bytes de índices) o None."""
    if len(b) < 128 or b[0] != 10:
        return None
    enc, bpp = b[2], b[3]
    x0, y0, x1, y1 = struct.unpack_from("<4H", b, 4)
    w, h = x1 - x0 + 1, y1 - y0 + 1
    planes, bpl = b[65], struct.unpack_from("<H", b, 66)[0]
    if w <= 0 or h <= 0 or w > 1024 or h > 1024:
        return None
    data = bytearray()
    p, need = 128, bpl * planes * h
    while len(data) < need and p < len(b):
        c = b[p]
        p += 1
        if enc and c >= 0xC0:
            if p >= len(b):
                break
            data += bytes([b[p]]) * (c & 0x3F)
            p += 1
        else:
            data.append(c)
    data += bytes(max(0, need - len(data)))   # relleno si el RLE llega truncado
    if bpp == 8 and planes == 1:
        pal = []
        if len(b) >= 769 and b[len(b) - 769] == 12:
            q = len(b) - 768
            pal = [(b[q + 3 * i], b[q + 3 * i + 1], b[q + 3 * i + 2]) for i in range(256)]
        else:
            pal = [(i, i, i) for i in range(256)]
        idx = bytearray()
        for y in range(h):
            idx += data[y * bpl:y * bpl + w]
        return w, h, pal, bytes(idx)
    pal = [tuple(b[16 + 3 * i:19 + 3 * i]) for i in range(16)]
    idx = bytearray()
    for y in range(h):
        row = data[y * bpl * planes:(y + 1) * bpl * planes]
        for x in range(w):
            v = 0
            if bpp == 1:
                for pl in range(planes):
                    v |= ((row[pl * bpl + (x >> 3)] >> (7 - (x & 7))) & 1) << pl
            elif bpp == 4 and planes == 1:
                byte = row[x >> 1]
                v = (byte >> 4) if (x & 1) == 0 else (byte & 15)
            idx.append(v)
    return w, h, pal, bytes(idx)


def build_tris(m, texlookup, uv_mode="norm"):
    """Aplana el modelo a triángulos en el espacio original del motor (y arriba), unidades originales. Cada triángulo: (p0,p1,p2, uv0,uv1,uv2, tex, n, objeto, tex_id).
    uv en 0..1: uv_mode 'norm' = byte/255 ; 'pix' = byte/ancho (se resuelve con la textura)."""
    offs = world_offsets(m)
    out = []
    tcache = {}

    def tl(t):
        if t not in tcache:
            tcache[t] = texlookup(t)
        return tcache[t]
    for oi, o in enumerate(m["objs"]):
        ox, oy, oz = offs[oi]
        V = [(x + ox, y + oy, z + oz) for x, y, z in o["verts"]]
        for fc in o["faces"]:
            tex = tl(fc[3])
            for t in tri_list(fc):
                pp = [V[fc[0][k]] for k in t]
                uv = []
                for k in t:
                    u, v = fc[2][k]
                    if uv_mode == "pix" and tex:
                        uv.append((u / tex[0], v / tex[1]))
                    else:
                        uv.append((u / 255.0, v / 255.0))
                a = [pp[1][i] - pp[0][i] for i in range(3)]
                b = [pp[2][i] - pp[0][i] for i in range(3)]
                n = (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])
                l = (n[0] ** 2 + n[1] ** 2 + n[2] ** 2) ** 0.5 or 1
                out.append((pp[0], pp[1], pp[2], uv[0], uv[1], uv[2], tex, (n[0] / l, n[1] / l, n[2] / l), oi, fc[3]))
    return out
