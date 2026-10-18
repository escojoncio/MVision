#!/usr/bin/env python3
"""Hojas de contacto y comparaciones a partir de los PPM que escribe camtest (sin dependencias del juego).
  sheet.py compare DIR SALIDA.png T1,T2,...      filas: ORIGINAL (o_) | VR sin filtro (e_) | VR con filtro (ef_)  a la pose del motor
  sheet.py model   DIR SALIDA.png T1,T2,...      validación del filtro: original sin filtrar (q_) | mi filtro (qf_) | original filtrado (o_)
  sheet.py grid    DIR SALIDA.png PREFIJO T1,T2,... COLS [ETIQUETAS|separadas]   cuadrícula de PPM PREFIJO_tNNNN
  sheet.py err     DIR T1,T2,...                  error medio |o - X| y medias RGB (excluye la zona de HUD) para q/qf/e/ef
Requiere Pillow y numpy."""
import sys
from PIL import Image, ImageDraw
import numpy as np


def load(d, p, t):
    try:
        return Image.open('%s/%s_t%04d.ppm' % (d, p, t)).convert('RGB')
    except Exception:
        return Image.new('RGB', (320, 256), (60, 0, 0))


def rows(d, out, ts, cols):
    S = None
    for i, t in enumerate(ts):
        ims = []
        for pre, lab in cols:
            im = load(d, pre, t).resize((640, 512), Image.NEAREST)
            ImageDraw.Draw(im).text((6, 4), '%s t=%d' % (lab, t), fill=(255, 255, 0))
            ims.append(im)
        if S is None:
            S = Image.new('RGB', (640 * len(cols), 512 * len(ts)))
        for j, im in enumerate(ims):
            S.paste(im, (j * 640, i * 512))
    S.save(out)
    print(out, S.size)


def grid(d, out, pre, ts, ncols, labs=None, w=400, h=320):
    n = len(ts)
    nr = (n + ncols - 1) // ncols
    S = Image.new('RGB', (ncols * w, nr * h))
    for i, t in enumerate(ts):
        im = load(d, pre, t).resize((w, h))
        ImageDraw.Draw(im).text((6, 4), labs[i] if labs else 't=%d' % t, fill=(255, 255, 0))
        S.paste(im, ((i % ncols) * w, (i // ncols) * h))
    S.save(out)
    print(out, S.size)


def err(d, ts):
    for t in ts:
        o = np.array(load(d, 'o', t)).astype(float)
        m = np.ones(o.shape[:2], bool)
        m[0:50, 0:150] = 0
        m[10:72, 230:310] = 0   # vida/oxígeno y radar del juego
        print('t=%d original RGB medio %s' % (t, np.round(o[m].mean(0), 1)))
        for p in ('q', 'qf', 'e', 'ef'):
            a = np.array(load(d, p, t)).astype(float)
            print('   %-3s MAD=%5.2f RGB medio %s' % (p, abs(o - a)[m].mean(), np.round(a[m].mean(0), 1)))


if __name__ == '__main__':
    c = sys.argv[1]
    if c == 'compare':
        rows(sys.argv[2], sys.argv[3], [int(x) for x in sys.argv[4].split(',')], [('o', 'ORIGINAL'), ('e', 'VR sin filtro'), ('ef', 'VR con filtro')])
    elif c == 'model':
        rows(sys.argv[2], sys.argv[3], [int(x) for x in sys.argv[4].split(',')], [('q', 'orig. sin filtrar'), ('qf', 'filtro modelado'), ('o', 'orig. filtrado')])
    elif c == 'grid':
        grid(sys.argv[2], sys.argv[3], sys.argv[4], [int(x) for x in sys.argv[5].split(',')], int(sys.argv[6]), sys.argv[7].split('|') if len(sys.argv) > 7 else None)
    elif c == 'err':
        err(sys.argv[2], [int(x) for x in sys.argv[3].split(',')])
    else:
        print(__doc__)
