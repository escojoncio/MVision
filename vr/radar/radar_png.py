#!/usr/bin/env python3
"""Convierte los PPM de una carpeta a PNG (sin dependencias salvo Pillow) y compone una hoja con los pares."""
import sys, os
from PIL import Image
d = sys.argv[1]
for n in sorted(os.listdir(d)):
    if n.endswith('.ppm'):
        Image.open(os.path.join(d, n)).save(os.path.join(d, n[:-4] + '.png'))
