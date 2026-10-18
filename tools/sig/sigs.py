#!/usr/bin/env python3
"""sigs: firmas de funciones MIPS R3000 sin código (solo hashes enmascarados).

  sigs.py make  <ref.exe> <ref.map> <salida.json>   # en CI: desde la build de referencia
  sigs.py match <firmas.json> <objetivo.exe> <salida.json>  # local: mapa nombre->dirección en el objetivo

Enmascarado: inmediatos de 16 bits de instrucciones I-type que suelen llevar
direcciones/offsets globales (lui, addiu, ori, loads/stores) y destino de j/jal.
Nunca se guardan bytes de código: solo sha1 del flujo enmascarado + tamaño + prefijo.
"""
import hashlib
import json
import re
import struct
import sys

HDR = 0x800
PREFIX_WORDS = 8
MASK_IMM_OPS = {0x08, 0x09, 0x0D, 0x0F, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x28, 0x29, 0x2A, 0x2B, 0x2E, 0x32, 0x3A}


def load_exe(path):
    d = open(path, "rb").read()
    if d[:8] != b"PS-X EXE":
        raise SystemExit(f"{path}: no es PS-X EXE")
    base, size = struct.unpack_from("<II", d, 0x18)
    if HDR + size > len(d):
        raise SystemExit(f"{path}: truncado")
    return base, d[HDR:HDR + size]


def mask(w):
    op = w >> 26
    if op in (2, 3):  # j / jal
        return w & 0xFC000000
    if op in MASK_IMM_OPS:
        return w & 0xFFFF0000
    return w


def words(text, off, n):
    return [mask(x) for x in struct.unpack_from(f"<{n}I", text, off)]


def h(ws):
    return hashlib.sha1(struct.pack(f"<{len(ws)}I", *ws)).hexdigest()


SECT_RE = re.compile(r"^\s*([0-9A-Fa-f]{8})\s+([0-9A-Fa-f]{8})\s+([0-9A-Fa-f]{8})\s+([0-9A-Fa-f]{8})\s+(\w+)\s+(\S+)\s*$")
SYM_RE = re.compile(r"^\s*([0-9A-Fa-f]{8})\s+(\S+)\s*$")


def parse_map(path, base, end):
    """Símbolos globales dentro de secciones .text del ejecutable (formato psylink).
    Devuelve [(addr, nombre, fin_de_su_sección)]."""
    texts, syms, in_syms = [], {}, False
    for line in open(path, errors="replace"):
        if "Names in address order" in line:
            in_syms = True
            continue
        if in_syms:
            m = SYM_RE.match(line)
            if m:
                syms.setdefault(int(m.group(1), 16), m.group(2))
            continue
        m = SECT_RE.match(line)
        if m and m.group(5) == "text" and int(m.group(3), 16):
            a, b = int(m.group(1), 16), int(m.group(2), 16) + 1
            if base <= a and b <= end:
                texts.append((a, b))
    if not texts:
        raise SystemExit("map sin secciones text dentro del exe")
    out = []
    for a, name in sorted(syms.items()):
        for ta, tb in texts:
            if ta <= a < tb:
                out.append((a, name, tb))
                break
    return out


def cmd_make(exe, mp, out):
    base, text = load_exe(exe)
    end = base + len(text)
    syms = parse_map(mp, base, end)
    res = []
    for i, (a, name, sect_end) in enumerate(syms):
        nxt = min(syms[i + 1][0], sect_end) if i + 1 < len(syms) else sect_end
        n = (nxt - a) // 4
        if n < PREFIX_WORDS:
            continue
        ws = words(text, a - base, n)
        res.append({"name": name, "addr": f"{a:08x}", "words": n,
                    "prefix": h(ws[:PREFIX_WORDS]), "full": h(ws)})
    json.dump(res, open(out, "w"), indent=0)
    print(f"{len(res)} firmas -> {out}")


def cmd_match(sigf, exe, out):
    sigs = json.load(open(sigf))
    base, text = load_exe(exe)
    nw = len(text) // 4
    allw = [mask(x) for x in struct.unpack_from(f"<{nw}I", text, 0)]
    index = {}
    for i in range(nw - PREFIX_WORDS + 1):
        index.setdefault(h(allw[i:i + PREFIX_WORDS]), []).append(i)
    found, amb, miss = {}, 0, 0
    for s in sigs:
        cands = [i for i in index.get(s["prefix"], [])
                 if i + s["words"] <= nw and h(allw[i:i + s["words"]]) == s["full"]]
        if len(cands) == 1:
            found[s["name"]] = {"ref": s["addr"], "target": f"{base + cands[0] * 4:08x}"}
        elif cands:
            amb += 1
        else:
            miss += 1
    json.dump(found, open(out, "w"), indent=1)
    print(f"exactas {len(found)} | ambiguas {amb} | sin match {miss} | total {len(sigs)}")


if __name__ == "__main__":
    a = sys.argv[1:]
    if len(a) == 4 and a[0] == "make":
        cmd_make(*a[1:])
    elif len(a) == 4 and a[0] == "match":
        cmd_match(*a[1:])
    else:
        print(__doc__)
        sys.exit(1)
