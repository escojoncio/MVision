#!/usr/bin/env python3
"""boxres: extrae del archivo de stages del usuario la máscara 2D de la vista interior y la textura de cartón,
y renderiza el aspecto ORIGINAL (máscara opaca + degradado de resta) sobre un panorama de prueba.
Salida fuera del repo. Los nombres/ids se pasan por argumento (nada del juego en el código).

  boxres.py <archivo_stages> <dir_salida> --stage <stage> --sight <id16 hex> --tex <id16 hex> --pano <pano.ppm>
Genera: sight.bin, tex.png (+ x20), original_t{-1..8}.png (lienzo 320x256 x2), original_sheet.png, sight_info.txt
Formato de la máscara: ver vr/box/box_data.h.  Proyección del juego: focal 320 px, centro óptico en la fila 120.
"""
import os, struct, sys
import numpy as np
from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "wmodel"))
sys.path.insert(0, os.path.join(HERE, "..", "stage"))
import mdlfmt as M  # noqa: E402
import stages  # noqa: E402


def load_ppm(p):
    d = open(p, "rb").read()
    parts = d.split(b"\n", 3)
    w, h = map(int, parts[1].split())
    return np.frombuffer(parts[3], np.uint8).reshape(h, w, 3).astype(np.float32)


def parse(b):
    n = b[3]
    eo, po, io, ro = struct.unpack_from("<4I", b, 4)
    prims = []
    for i in range(n):
        pid, fl, off = struct.unpack_from("<BBH", b, eo + 4 * i)
        p = po + off
        code = b[p + 7]
        recs = []
        for k in range(4):
            ri = b[io + (pid - 1) * 4 + k]
            if ri:
                lo, hi, dx, dy = struct.unpack_from("<BBbb", b, ro + (ri - 1) * 4)
                recs.append((int((lo - hi) * dx / 4), int((lo - hi) * dy / 4)))
            else:
                recs.append((0, 0))
        if code == 0x28:
            xy = [struct.unpack_from("<hh", b, p + 8 + 4 * k) for k in range(4)]
            prims.append(("F4", xy, recs, None))
        else:
            xy = [struct.unpack_from("<hh", b, p + 8 + 8 * k) for k in range(4)]
            cols = [min(b[p + 4:p + 7])] + [min(b[p + 12 + 8 * k:p + 15 + 8 * k]) for k in range(3)]
            prims.append(("G4", xy, recs, cols))
    return prims


def render_sight(bg, prims, o):
    """bg: HxWx3 float (lienzo 256x320); o = 0 cerrado .. 1 abierto. Devuelve la imagen con la máscara."""
    H, W, _ = bg.shape
    img = bg.copy()
    k = 1.0 - o
    mask = Image.new("L", (W, H), 0)
    d = ImageDraw.Draw(mask)
    for kind, xy, recs, cols in prims:
        pts = [(x + sx * k, y + sy * k) for (x, y), (sx, sy) in zip(xy, recs)]
        if kind == "F4":
            d.polygon([pts[0], pts[1], pts[3], pts[2]], fill=255)
    img[np.array(mask) > 0] = 0
    yy, xx = np.mgrid[0:H, 0:W]
    sub = np.zeros((H, W), np.float32)
    for kind, xy, recs, cols in prims:
        if kind != "G4":
            continue
        pts = [(x + sx * k, y + sy * k) for (x, y), (sx, sy) in zip(xy, recs)]
        for tri in ((0, 1, 2), (1, 2, 3)):
            (x0, y0), (x1, y1), (x2, y2) = [pts[i] for i in tri]
            den = (y1 - y2) * (x0 - x2) + (x2 - x1) * (y0 - y2)
            if abs(den) < 1e-6:
                continue
            l0 = ((y1 - y2) * (xx + .5 - x2) + (x2 - x1) * (yy + .5 - y2)) / den
            l1 = ((y2 - y0) * (xx + .5 - x2) + (x0 - x2) * (yy + .5 - y2)) / den
            l2 = 1 - l0 - l1
            m = (l0 >= 0) & (l1 >= 0) & (l2 >= 0)
            v = l0 * cols[tri[0]] + l1 * cols[tri[1]] + l2 * cols[tri[2]]
            sub = np.where(m, v, sub)
    img = np.clip(img - sub[..., None], 0, 255)
    return img


def game_view(pano, W=320, H=256, f=320.0, cy=120.0):
    ph, pw, _ = pano.shape
    yy, xx = np.mgrid[0:H, 0:W]
    x = (xx + .5 - W / 2) / f
    y = -(yy + .5 - cy) / f
    z = -np.ones_like(x)
    n = np.sqrt(x * x + y * y + 1)
    lon = np.arctan2(x, -z)
    lat = np.arcsin(y / n)
    u = ((lon + np.pi) / (2 * np.pi) * pw).astype(int) % pw
    v = np.clip(((np.pi / 2 - lat) / np.pi * ph).astype(int), 0, ph - 1)
    return pano[v, u]


def main(a):
    if len(a) < 2:
        print(__doc__)
        return 1
    opt = lambda k: a[a.index(k) + 1]  # noqa: E731
    d = open(a[0], "rb").read()
    out = a[1]
    os.makedirs(out, exist_ok=True)
    tb = dict(stages.table(d))
    c, bl = M.stage_blocks(d, tb, stages, opt("--stage"))
    sid, tid = int(opt("--sight"), 16), int(opt("--tex"), 16)
    sight = tex = None
    for i, x, b in c + bl:
        if x == "s" and (i & 0xFFFF) == sid:
            sight = b
        if x == "p" and (i & 0xFFFF) == tid:
            tex = b
    open(os.path.join(out, "sight.bin"), "wb").write(sight)
    r = M.decode_img(tex)
    w, h, pal, idx = r
    im = Image.new("RGB", (w, h))
    im.putdata([pal[i] for i in idx])
    im.save(os.path.join(out, "tex.png"))
    im.resize((w * 20, h * 20), Image.NEAREST).save(os.path.join(out, "tex_x20.png"))
    prims = parse(sight)
    f4 = [p for p in prims if p[0] == "F4"]
    inner = [(x, y) for p in f4 for (x, y) in p[1] if 0 < x < 320 and 0 < y < 256]
    xs, ys = [p[0] for p in inner], [p[1] for p in inner]
    info = (f"primitivas: {len(f4)} planas + {len(prims) - len(f4)} Gouraud\n"
            f"hueco final: x {min(xs)}..{max(xs)} (ancho {max(xs) - min(xs)}), y {min(ys)}..{max(ys)} (alto {max(ys) - min(ys)}) "
            f"centro ({(min(xs) + max(xs)) / 2},{(min(ys) + max(ys)) / 2}) en lienzo 320x256\n"
            f"ancho {100 * (max(xs) - min(xs)) / 320:.1f}% de 320, alto {100 * (max(ys) - min(ys)) / 240:.1f}% de 240\n")
    W = 320.0
    info += ("angulos con foco 320 px, centro optico (160,120): izq %.1f dcha %.1f arriba %.1f abajo %.1f grados\n" % (
        np.degrees(np.arctan((160 - min(xs)) / W)), np.degrees(np.arctan((max(xs) - 160) / W)),
        np.degrees(np.arctan((120 - min(ys)) / W)), np.degrees(np.arctan((max(ys) - 120) / W))))
    open(os.path.join(out, "sight_info.txt"), "w").write(info)
    print(info)
    pano = load_ppm(opt("--pano"))
    bg = game_view(pano)
    frames = []
    for t, o in [(-1, 0.0), (2, 0.25), (4, 0.5), (6, 0.75), (8, 1.0)]:
        im2 = render_sight(bg, prims, o).astype(np.uint8)
        im2 = Image.fromarray(im2).resize((640, 512), Image.NEAREST)
        im2.save(os.path.join(out, f"original_t{t}.png"))
        frames.append(im2)
    Image.fromarray(bg.astype(np.uint8)).resize((640, 512), Image.NEAREST).save(os.path.join(out, "original_nomask.png"))
    sheet = Image.new("RGB", (640 * 3, 512 * 2))
    for k, fr in enumerate(frames[:1] + frames[2:3] + frames[4:5]):
        sheet.paste(fr, (640 * k, 0))
    sheet.save(os.path.join(out, "original_sheet.png"))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
