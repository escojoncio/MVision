#!/usr/bin/env python3
"""Parche de recorte del ejecutable (inmediatos de comparación de pantalla) para la cámara VR.

El motor descarta objetos, polígonos y subdivisiones por su posición en pantalla comparando coordenadas
proyectadas con una caja fija (media anchura 160 px, media altura 128 px en el modo de 320x256 de la edición
europea; la caja de "punto visible" usa 192 x 152). Con la cabeza del usuario como cámara hace falta cubrir
mucho más campo: aquí se ensancha esa caja SIN tocar la proyección (la distancia de pantalla, H, sigue siendo la
del juego; la proyección por ojo la hace el renderer con las posiciones 3D de vista).

Cada punto de parche es (dirección, opcode MIPS esperado, inmediato antiguo, papel). Antes de escribir se
comprueba que la palabra de esa dirección tiene ese opcode y ese inmediato: si no coincide, aborta (otra
edición/versión del ejecutable). No se guarda ninguna instrucción del juego, solo opcode (hecho del ISA) e
inmediato. El resultado es un ejecutable con el mismo tamaño que se recompila como siempre
(`make gen EXE=...`); el original no se modifica.

Uso:
  patch_exe.py ORIGINAL SALIDA [--hfov 140] [--vfov 120] [--h 160] [--margin 1.0] [--no-divide] [--wide-points]
  patch_exe.py ORIGINAL --check            comprueba los puntos sin escribir
  patch_exe.py --table                     imprime la tabla de puntos

hfov/vfov = campo TOTAL en grados que debe cubrir el recorte (alrededor de la dirección de la cámara del motor);
h = distancia de pantalla H del motor con la que se calcula la caja (defecto 160 = VrCamConfig.engine_h; el juego usa 320).
Los 12 sitios de "punto visible" (reubicación de enemigos por ruta, órdenes de guion "¿en pantalla?", cámara de fotos)
son reglas del juego ligadas a la cámara: NO se tocan salvo con --wide-points (solo pruebas). Se comprueban siempre.
SALIDA nunca puede ser ORIGINAL ni un fichero dentro de un repo git que no esté ignorado (el EXE parcheado no se sube).
La caja se limita a 1020 px (la GTE satura SX/SY a +-1024: más allá no hay coordenada fiable).

--no-divide: desactiva la subdivisión de polígonos cercanos del motor (etapa "divide"). Ese paso calcula los puntos
medios de cada polígono grande y cercano con la GTE, pero luego desplaza +-1 px el SXY entero de los vértices nuevos
(para ocultar costuras), de modo que el renderer no puede enlazarlos con su posición 3D de vista. Con la reproyección en
coma flotante y por pixel no hace falta subdividir. Es global (también sin cámara VR): solo afecta a la deformación
afín de texturas del dibujado nativo cuando la cámara VR no está activa.
"""
import math
import struct
import sys

# Firma de 8 bytes de la cabecera del formato de ejecutable (hex).
EXE_MAGIC = bytes.fromhex("50532d5820455845")

OP_ADDIU, OP_SLTI, OP_SLTIU = 0x09, 0x0A, 0x0B

# rol: HX+1 ("<161": borde derecho), -HX (borde izquierdo), HY+1, -HY (vertical), P* = caja de "punto visible"
#      (suma de centrado y límite sin signo)
ROLE_HX1, ROLE_NHX, ROLE_HY1, ROLE_NHY = "hx+1", "-hx", "hy+1", "-hy"
ROLE_PX, ROLE_PX2, ROLE_PY, ROLE_PY2 = "px", "2px+1", "py", "2py+1"
OLD = {ROLE_HX1: (OP_SLTI, 161), ROLE_NHX: (OP_SLTI, -160), ROLE_HY1: (OP_SLTI, 129), ROLE_NHY: (OP_SLTI, -128),
       ROLE_PX: (OP_ADDIU, 192), ROLE_PX2: (OP_SLTIU, 385), ROLE_PY: (OP_ADDIU, 152), ROLE_PY2: (OP_SLTIU, 305)}

# (dirección, papel). Grupos: recorte de cajas envolventes (2 copias), código de salida de la subdivisión,
# marcas de vértice fuera de pantalla al transformar (6 copias), "punto visible" (3 comprobaciones).
SITES = []
def _quad(a0, a1, a2, a3, a4, a5, a6, a7):
    # orden en el código: x:<161 / -160  y:<129 / -128, dos veces (fuera de pantalla / parcial)
    return [(a0, ROLE_HX1), (a1, ROLE_NHX), (a2, ROLE_HY1), (a3, ROLE_NHY),
            (a4, ROLE_HX1), (a5, ROLE_NHX), (a6, ROLE_HY1), (a7, ROLE_NHY)]
SITES += _quad(0x80018b40, 0x80018b48, 0x80018b54, 0x80018b5c, 0x80018b68, 0x80018b70, 0x80018b78, 0x80018b80)
SITES += _quad(0x80018f54, 0x80018f5c, 0x80018f64, 0x80018f6c, 0x80018f74, 0x80018f7c, 0x80018f84, 0x80018f8c)
SITES += [(0x800194f4, ROLE_NHX), (0x80019508, ROLE_HX1), (0x80019520, ROLE_NHY), (0x80019528, ROLE_HY1)]
# trans: seis copias de (-160, 161, -128, 129)
SITES += [(0x8001e4b8, ROLE_NHX), (0x8001e4cc, ROLE_HX1), (0x8001e4e4, ROLE_NHY), (0x8001e4ec, ROLE_HY1),
          (0x8001e510, ROLE_NHX), (0x8001e524, ROLE_HX1), (0x8001e53c, ROLE_NHY), (0x8001e544, ROLE_HY1),
          (0x8001e56c, ROLE_NHX), (0x8001e580, ROLE_HX1), (0x8001e598, ROLE_NHY), (0x8001e5a0, ROLE_HY1),
          (0x8001e608, ROLE_NHX), (0x8001e61c, ROLE_HX1), (0x8001e634, ROLE_NHY), (0x8001e63c, ROLE_HY1),
          (0x8001e664, ROLE_NHX), (0x8001e678, ROLE_HX1), (0x8001e690, ROLE_NHY), (0x8001e698, ROLE_HY1),
          (0x8001e6c0, ROLE_NHX), (0x8001e6d4, ROLE_HX1), (0x8001e6ec, ROLE_NHY), (0x8001e6f4, ROLE_HY1)]
SITES += [(0x8001c440, ROLE_PX), (0x8001c444, ROLE_PX2), (0x8001c458, ROLE_PY), (0x8001c45c, ROLE_PY2),
          (0x8001c4d8, ROLE_PX), (0x8001c4dc, ROLE_PX2), (0x8001c4f0, ROLE_PY), (0x8001c4f4, ROLE_PY2),
          (0x8001c5e8, ROLE_PX), (0x8001c5ec, ROLE_PX2), (0x8001c600, ROLE_PY), (0x8001c604, ROLE_PY2)]

# Parches de una instrucción (dirección, palabra esperada, palabra nueva, nombre del interruptor).
# 0x8001a11c: `addiu v0,zero,0x800` -> `lui v0,0x7FFF`: umbral de área a partir del cual se subdivide (guardado en el
# bloque de trabajo de la etapa "divide"); con 0x7FFF0000 no se subdivide nunca.
WORD_PATCHES = {"no-divide": [(0x8001A11C, 0x24020800, 0x3C027FFF)]}

LIMIT = 1020  # la GTE satura SX/SY en [-1024, 1023]


def box(h, hfov, vfov, margin):  # h = H del motor
    """Media caja (px) que cubre hfov/vfov grados totales con distancia de pantalla h (x margen)."""
    hx = min(LIMIT, int(round(h * math.tan(math.radians(hfov * margin) / 2))))
    hy = min(LIMIT, int(round(h * math.tan(math.radians(vfov * margin) / 2))))
    return max(hx, 160), max(hy, 128)


def new_value(role, hx, hy):
    return {ROLE_HX1: hx + 1, ROLE_NHX: -hx, ROLE_HY1: hy + 1, ROLE_NHY: -hy,
            ROLE_PX: hx, ROLE_PX2: 2 * hx + 1, ROLE_PY: hy, ROLE_PY2: 2 * hy + 1}[role]


def output_ok(src, dst):
    """False si dst es el original o está dentro de un repo git sin estar ignorado (nunca se escribe un EXE parcheado
    donde pueda subirse). Fuera de cualquier repo git, o dentro pero ignorado (p. ej. build/), se permite."""
    import os, subprocess
    if os.path.realpath(src) == os.path.realpath(dst):
        print("SALIDA no puede ser el ejecutable original"); return False
    d = os.path.dirname(os.path.realpath(dst)) or "."
    os.makedirs(d, exist_ok=True)
    try:
        top = subprocess.run(["git", "-C", d, "rev-parse", "--show-toplevel"], capture_output=True, text=True)
    except OSError:
        return True  # sin git instalado: no se puede comprobar
    if top.returncode != 0:
        return True  # fuera de un repo
    ign = subprocess.run(["git", "-C", d, "check-ignore", "-q", os.path.realpath(dst)])
    if ign.returncode == 0:
        return True
    print(f"{dst}: dentro del repo git {top.stdout.strip()} y no ignorado; no se escribe el EXE parcheado"); return False


def load(path):
    d = bytearray(open(path, "rb").read())
    if d[:8] != EXE_MAGIC:
        sys.exit("ejecutable no válido")
    base, size = struct.unpack_from("<II", d, 0x18)[0], struct.unpack_from("<I", d, 0x1C)[0]
    return d, base, size


def check(d, base, size):
    bad = 0
    for addr, role in SITES:
        off = 0x800 + addr - base
        if not (0 <= off <= 0x800 + size - 4):
            print(f"{addr:08x}: fuera del texto"); bad += 1; continue
        w = struct.unpack_from("<I", d, off)[0]
        op, imm = w >> 26, w & 0xFFFF
        imm = imm - 0x10000 if imm >= 0x8000 else imm
        eop, eimm = OLD[role]
        if op != eop or imm != eimm:
            print(f"{addr:08x}: esperado op {eop:#x} imm {eimm}, hay op {op:#x} imm {imm}"); bad += 1
    for _, lst in WORD_PATCHES.items():
        for addr, old, _new in lst:
            off = 0x800 + addr - base
            if not (0 <= off <= 0x800 + size - 4):
                print(f"{addr:08x}: fuera del texto"); bad += 1; continue
            w = struct.unpack_from("<I", d, off)[0]
            if w != old:
                print(f"{addr:08x}: esperado {old:08x}, hay {w:08x}"); bad += 1
    return bad


def main(a):
    if "--table" in a:
        for addr, role in SITES:
            print(f"{addr:08x} {role}")
        return 0
    if not a:
        print(__doc__); return 1
    src = a[0]
    d, base, size = load(src)
    bad = check(d, base, size)
    if bad:
        print(f"{bad} puntos no coinciden: ejecutable distinto del esperado, no se parchea")
        return 2
    if "--check" in a:
        print(f"{len(SITES)} puntos de recorte + {sum(len(l) for l in WORD_PATCHES.values())} de subdivisión coinciden"); return 0
    if len(a) < 2:
        print("falta SALIDA"); return 1
    opt = lambda k, dflt: float(a[a.index(k) + 1]) if k in a else dflt
    h, hfov, vfov, margin = opt("--h", 160.0), opt("--hfov", 140.0), opt("--vfov", 120.0), opt("--margin", 1.0)
    hx, hy = box(h, hfov, vfov, margin)
    wide = "--wide-points" in a
    if not output_ok(src, a[1]):
        return 3
    patched = 0
    for addr, role in SITES:
        if role in (ROLE_PX, ROLE_PX2, ROLE_PY, ROLE_PY2) and not wide:
            continue
        patched += 1
        off = 0x800 + addr - base
        w = struct.unpack_from("<I", d, off)[0]
        struct.pack_into("<I", d, off, (w & 0xFFFF0000) | (new_value(role, hx, hy) & 0xFFFF))
    extra = []
    for name, lst in WORD_PATCHES.items():
        if f"--{name}" in a:
            for addr, _old, new in lst:
                struct.pack_into("<I", d, 0x800 + addr - base, new)
            extra.append(name)
    import os
    tmp = a[1] + ".tmp"
    with open(tmp, "wb") as f:
        f.write(d)
    os.replace(tmp, a[1])   # atómico: nunca queda un EXE parcheado a medias
    print(f"parche: {patched} inmediatos, caja +-{hx} x +-{hy} px (H={h:g}, campo {hfov:g} x {vfov:g} grados)" + (f" + {', '.join(extra)}" if extra else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
