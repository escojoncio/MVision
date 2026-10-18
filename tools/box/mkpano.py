#!/usr/bin/env python3
"""mkpano: genera un panorama equirrectangular procedural (escena de fondo para las pruebas visuales de la
carcasa de visión interior). Sin datos del juego. Solo PIL + numpy.

  mkpano.py <salida.ppm> [<salida.png>] [--w 2048]

Convención: lon = 0 es -Z (hacia delante), crece hacia +X (derecha); lat = 0 es el horizonte, +Y arriba.
Ojo a 1,6 m del suelo. Marcas: poste rojo al frente, azules a +-20 grados, amarillos a +-40, líneas cada 15 grados.
"""
import sys
import numpy as np
from PIL import Image


def build(W):
    H = W // 2
    lon = (np.arange(W) + 0.5) / W * 2 * np.pi - np.pi          # -pi..pi, 0 = delante
    lat = np.pi / 2 - (np.arange(H) + 0.5) / H * np.pi
    LON, LAT = np.meshgrid(lon, lat)
    dx = np.sin(LON) * np.cos(LAT)
    dy = np.sin(LAT)
    dz = -np.cos(LON) * np.cos(LAT)
    img = np.zeros((H, W, 3), np.float32)
    hor = np.array([205, 218, 232], np.float32)
    zen = np.array([60, 110, 185], np.float32)
    k = np.clip(LAT / (np.pi / 2), 0, 1)[..., None] ** 0.6
    img[:] = hor * (1 - k) + zen * k
    # suelo
    eye = 1.6
    g = LAT < 0
    t = np.where(g, eye / np.maximum(-dy, 1e-4), 0)
    gx, gz = dx * t, dz * t
    chk = ((np.floor(gx / 2) + np.floor(gz / 2)) % 2)[..., None]
    gcol = np.array([104, 112, 98], np.float32) * (1 - chk) + np.array([78, 86, 74], np.float32) * chk
    line = ((np.abs(gx - np.round(gx / 2) * 2) < 0.04) | (np.abs(gz - np.round(gz / 2) * 2) < 0.04))[..., None]
    gcol = np.where(line, gcol * 0.7, gcol)
    fog = np.clip(t / 60, 0, 1)[..., None]
    gcol = gcol * (1 - fog) + hor * fog
    img = np.where(g[..., None], gcol, img)
    # edificios: (lon0, lon1 en grados, distancia, altura, color)
    blds = [(-100, -80, 14, 9, (150, 120, 100)), (-70, -50, 22, 14, (120, 130, 140)), (-48, -30, 10, 5, (170, 160, 140)),
            (-26, -12, 30, 18, (110, 116, 128)), (-8, 10, 18, 7, (160, 140, 120)), (14, 30, 12, 11, (130, 100, 90)),
            (34, 52, 26, 16, (118, 124, 134)), (56, 80, 9, 6, (176, 150, 120)), (84, 110, 20, 12, (140, 140, 150)),
            (130, 160, 16, 10, (150, 125, 105)), (-160, -130, 24, 15, (125, 130, 140))]
    deg = np.degrees(LON)
    for a0, a1, d, h, col in blds:
        m = (deg >= a0) & (deg <= a1)
        # elevación de la base y del tejado vistas desde el ojo
        lo, hi = np.arctan2(-eye, d), np.arctan2(h - eye, d)
        # distancia real a la pared depende del azimut; se aproxima plana
        mm = m & (LAT >= lo) & (LAT <= hi)
        u = (deg - a0) / max(a1 - a0, 1) * 8
        v = (np.tan(LAT) * d + eye) / 2.0
        win = ((u % 1) > 0.25) & ((u % 1) < 0.75) & ((v % 1) > 0.3) & ((v % 1) < 0.8)
        shade = np.where(win, 0.55, 1.0)[..., None]
        img = np.where(mm[..., None], np.array(col, np.float32) * shade, img)
    # postes de referencia
    for ang, col in [(0, (220, 30, 30)), (20, (30, 60, 220)), (-20, (30, 60, 220)), (40, (230, 200, 30)), (-40, (230, 200, 30)),
                     (60, (30, 180, 60)), (-60, (30, 180, 60))]:
        d = 7.0
        m = (np.abs(deg - ang) < 0.9) & (LAT < np.arctan2(1.2, d)) & (LAT > np.arctan2(-eye, d))
        img = np.where(m[..., None], np.array(col, np.float32), img)
    # techo/cielo: nubes de bandas
    cl = (np.sin(LON * 7 + 1.3) * np.sin(LAT * 18) > 0.55) & (LAT > 0.25)
    img = np.where(cl[..., None], img * 0.4 + 255 * 0.6, img)
    # rejilla angular cada 15 grados (finas, semitransparentes)
    gl = (np.abs(((deg + 7.5) % 15) - 7.5) < 0.12) | (np.abs(((np.degrees(LAT) + 7.5) % 15) - 7.5) < 0.12)
    img = np.where(gl[..., None], img * 0.75 + 20, img)
    return np.clip(img, 0, 255).astype(np.uint8)


def main(a):
    if not a:
        print(__doc__)
        return 1
    W = int(a[a.index("--w") + 1]) if "--w" in a else 2048
    pos = [x for i, x in enumerate(a) if not x.startswith("--") and (i == 0 or a[i - 1] != "--w")]
    img = build(W)
    with open(pos[0], "wb") as f:
        f.write(b"P6\n%d %d\n255\n" % (img.shape[1], img.shape[0]))
        f.write(img.tobytes())
    if len(pos) > 1:
        Image.fromarray(img).save(pos[1])
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
