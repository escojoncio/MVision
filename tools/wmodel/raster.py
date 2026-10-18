"""raster: escritor PNG y rasterizador de software mínimo con textura (solo stdlib)."""
import math
import struct
import zlib


def write_png(path, w, h, rgb):
    """rgb: bytes/bytearray de w*h*3 (o *4 si len coincide con RGBA)."""
    ch = len(rgb) // (w * h)
    ct = 2 if ch == 3 else 6
    raw = bytearray()
    for y in range(h):
        raw.append(0)
        raw += rgb[y * w * ch:(y + 1) * w * ch]

    def chunk(t, d):
        c = struct.pack(">I", len(d)) + t + d
        return c + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, ct, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(raw), 6)) + chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(png)


def tex_rgba(img):
    """(w,h,pal,idx) -> bytes RGBA."""
    w, h, pal, idx = img
    out = bytearray()
    for i in idx:
        r, g, b = pal[i] if i < len(pal) else (0, 0, 0)
        out += bytes((r, g, b, 255))
    return bytes(out)


def render(tris, size=128, yaw=0.6, pitch=0.35, bg=(40, 44, 52)):
    """tris: lista de (p0,p1,p2, uv0,uv1,uv2, tex|None, n) con p en espacio modelo (y arriba).
    tex = (w,h,rgba). Devuelve bytes RGB size*size*3. Cámara ortográfica auto-encuadrada."""
    cy, sy, cp, sp = math.cos(yaw), math.sin(yaw), math.cos(pitch), math.sin(pitch)

    def xf(p):
        x = p[0] * cy + p[2] * sy
        z = -p[0] * sy + p[2] * cy
        y = p[1] * cp - z * sp
        z2 = p[1] * sp + z * cp
        return x, y, z2
    P = [[xf(p) for p in t[:3]] for t in tris]
    if not P:
        return bytes(bg) * (size * size)
    xs = [v[0] for t in P for v in t]
    ys = [v[1] for t in P for v in t]
    mnx, mxx, mny, mxy = min(xs), max(xs), min(ys), max(ys)
    sc = (size - 8) / max(mxx - mnx, mxy - mny, 1e-6)
    ox = size / 2 - (mnx + mxx) / 2 * sc
    oy = size / 2 + (mny + mxy) / 2 * sc
    img = bytearray(bytes(bg) * (size * size))
    zb = [-1e30] * (size * size)
    lx, ly, lz = 0.4, 0.7, 0.6
    for t, pr in zip(tris, P):
        a, b, c = pr
        pts = [(v[0] * sc + ox, -v[1] * sc + oy, v[2]) for v in (a, b, c)]
        (x0, y0, z0), (x1, y1, z1), (x2, y2, z2) = pts
        den = (y1 - y2) * (x0 - x2) + (x2 - x1) * (y0 - y2)
        if abs(den) < 1e-9:
            continue
        n = xf_n(t[7], cy, sy, cp, sp)
        shade = 0.45 + 0.55 * abs(n[0] * lx + n[1] * ly + n[2] * lz)
        tex = t[6]
        minx, maxx = max(0, int(min(x0, x1, x2))), min(size - 1, int(max(x0, x1, x2)) + 1)
        miny, maxy = max(0, int(min(y0, y1, y2))), min(size - 1, int(max(y0, y1, y2)) + 1)
        for y in range(miny, maxy + 1):
            for x in range(minx, maxx + 1):
                px, py = x + 0.5, y + 0.5
                w0 = ((y1 - y2) * (px - x2) + (x2 - x1) * (py - y2)) / den
                w1 = ((y2 - y0) * (px - x2) + (x0 - x2) * (py - y2)) / den
                w2 = 1 - w0 - w1
                if w0 < -0.02 or w1 < -0.02 or w2 < -0.02:
                    continue
                z = w0 * z0 + w1 * z1 + w2 * z2
                i = y * size + x
                if z <= zb[i]:
                    continue
                if tex:
                    tw, th, rgba = tex
                    u = (w0 * t[3][0] + w1 * t[4][0] + w2 * t[5][0])
                    v = (w0 * t[3][1] + w1 * t[4][1] + w2 * t[5][1])
                    tx = min(tw - 1, max(0, int(u * tw)))
                    ty = min(th - 1, max(0, int(v * th)))
                    o = (ty * tw + tx) * 4
                    r, g, bb = rgba[o], rgba[o + 1], rgba[o + 2]
                else:
                    r = g = bb = 170
                zb[i] = z
                img[i * 3] = int(r * shade)
                img[i * 3 + 1] = int(g * shade)
                img[i * 3 + 2] = int(bb * shade)
    return bytes(img)


def xf_n(n, cy, sy, cp, sp):
    x = n[0] * cy + n[2] * sy
    z = -n[0] * sy + n[2] * cy
    y = n[1] * cp - z * sp
    z2 = n[1] * sp + z * cp
    return x, y, z2


def sheet(images, size, cols, labels=None):
    """Junta imágenes RGB size*size en una hoja; devuelve (w,h,bytes)."""
    rows = (len(images) + cols - 1) // cols
    W, H = cols * size, rows * size
    out = bytearray(bytes((24, 26, 30)) * (W * H))
    for k, im in enumerate(images):
        cx, cy = (k % cols) * size, (k // cols) * size
        for y in range(size):
            out[((cy + y) * W + cx) * 3:((cy + y) * W + cx + size) * 3] = im[y * size * 3:(y + 1) * size * 3]
    return W, H, bytes(out)
