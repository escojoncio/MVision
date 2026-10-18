#!/usr/bin/env python3
"""render_compare: genera las imágenes de comparación original vs VR (usa boxprev y boxres.py ya ejecutado).
  render_compare.py <dir_trabajo> <archivo_stages> --stage s --sight 0x.. --tex 0x..
Requiere <dir>/pano.ppm (mkpano.py) y <dir>/original_*.png (boxres.py). Escribe <dir>/cmp_*.png."""
import os, subprocess, sys
from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
BIN = os.path.join(HERE, "..", "..", "vr", "box", "build", "boxprev")


def main(a):
    d, stages = a[0], a[1]
    opt = lambda k: a[a.index(k) + 1]  # noqa: E731
    base = [BIN, "--pano", f"{d}/pano.ppm", "--stages", stages, "--stage", opt("--stage"), "--sight", opt("--sight"), "--tex", opt("--tex")]

    def r(name, *x, aa="2"):
        p = f"{d}/{name}.ppm"
        subprocess.run(base + ["--out", p, "--aa", aa] + list(x), check=True, stderr=subprocess.DEVNULL)
        im = Image.open(p).convert("RGB")
        im.save(f"{d}/{name}.png")
        os.remove(p)
        return im

    def label(im, t):
        im = im.copy()
        ImageDraw.Draw(im).text((8, 6), t, fill=(255, 255, 0))
        return im

    g = ["--w", "640", "--h", "512", "--focal", "640", "--cx", "320", "--cy", "240", "--nolag"]
    orig = Image.open(f"{d}/original_t8.png").convert("RGB")
    card = r("vr_gamefov_card", *g)
    black = r("vr_gamefov_black", *g, "--style", "black")
    sheet = Image.new("RGB", (640 * 3, 512))
    for k, (im, t) in enumerate([(orig, "ORIGINAL (mascara 2D)"), (black, "VR estilo negro (fiel)"), (card, "VR estilo carton")]):
        sheet.paste(label(im, t), (640 * k, 0))
    sheet.save(f"{d}/cmp_original_vs_vr.png")

    # FOV de visor (110 grados horizontal), ojos izquierdo/derecho
    v = ["--w", "800", "--h", "800", "--focal", str(400 / 1.4281), "--nolag"]
    L = r("vr_wide_L", *v, "--eye", "l"); R = r("vr_wide_R", *v, "--eye", "r"); C = r("vr_wide_C", *v)
    s2 = Image.new("RGB", (2400, 800))
    for k, (im, t) in enumerate([(L, "ojo izquierdo"), (C, "centro"), (R, "ojo derecho")]):
        s2.paste(label(im, t), (800 * k, 0))
    s2.save(f"{d}/cmp_stereo_wide_fov.png")
    # accesibilidad
    W = r("vr_wide_access", *v, "--wide")
    s3 = Image.new("RGB", (1600, 800)); s3.paste(label(C, "normal"), (0, 0)); s3.paste(label(W, "abertura ancha (accesibilidad)"), (800, 0))
    s3.save(f"{d}/cmp_accessibility.png")
    # mirando de lado con la cabeza girada (la abertura sigue delante)
    Y = r("vr_yaw", *v, "--yaw", "55", "--pitch", "-10", "--nolag")
    # transición: bajada y apertura / salida
    fr = []
    for t in (0.08, 0.25, 0.38, 0.5, 0.65, 0.85):
        fr.append(label(r(f"vr_t{int(t * 100):03d}", *v, "--t", str(t), aa="1"), f"entrada t={t}s"))
    for t in (0.1, 0.3):
        fr.append(label(r(f"vr_leave{int(t * 100):03d}", *v, "--t", "2", "--leave", str(t), aa="1"), f"salida +{t}s"))
    fr.append(label(Y, "cabeza girada 55/-10"))
    cols = 3
    s4 = Image.new("RGB", (800 * cols, 800 * 3))
    for k, im in enumerate(fr):
        s4.paste(im, (800 * (k % cols), 800 * (k // cols)))
    s4.resize((s4.width // 2, s4.height // 2)).save(f"{d}/cmp_transition.png")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
