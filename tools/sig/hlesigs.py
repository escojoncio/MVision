#!/usr/bin/env python3
"""hlesigs: filtra sigs_ref.json al subconjunto que necesita el runtime sin código generado.

  hlesigs.py <sigs_ref.json> <salida.json> <hle1.txt> [hle2.txt ...] [--ctx N]

Entrada: firmas del ejecutable de referencia (sigs.py make; solo sha1 enmascarados + tamaño, nunca código).
Salida: las funciones listadas en runtime/hle*.txt (role "hle") más N vecinas por lado en el orden de la
referencia (role "anchor", por defecto 4), que son las anclas con las que hlebind.c valida los emparejamientos
por distancia/orden. Formato y algoritmo de emparejamiento: cabecera de runtime/hlebind.c.
El fichero resultante va dentro del IPA (no se sube al repo): se genera en CI (job `reference`).
"""
import json
import sys


def main(a):
    ctx = 4
    if "--ctx" in a:
        i = a.index("--ctx")
        ctx = int(a[i + 1])
        del a[i:i + 2]
    if len(a) < 3:
        print(__doc__)
        return 1
    sigs = sorted(json.load(open(a[0])), key=lambda s: int(s["addr"], 16))
    names = set()
    for f in a[2:]:
        for line in open(f):
            n = line.split("#")[0].strip()
            if n:
                names.add(n)
    keep = {}
    # Gemelas en la referencia COMPLETA: cuántas funciones comparten el hash completo / de prefijo. El runtime
    # no empareja por hash único funciones con gemelas (un único candidato en el EXE no las distingue).
    fcount, pcount = {}, {}
    for s in sigs:
        fcount[s["full"]] = fcount.get(s["full"], 0) + 1
        pcount[s["prefix"]] = pcount.get(s["prefix"], 0) + 1
    idx = {s["name"]: i for i, s in enumerate(sigs)}
    missing = sorted(n for n in names if n not in idx)
    for n in names:
        i = idx.get(n)
        if i is None:
            continue
        keep[i] = "hle"
        for j in range(max(0, i - ctx), min(len(sigs), i + ctx + 1)):
            keep.setdefault(j, "anchor")
    out = []
    for i in sorted(keep):
        s = dict(sigs[i])
        s["role"] = keep[i]
        s["fd"], s["pd"] = fcount[s["full"]], pcount[s["prefix"]]
        out.append(s)
    with open(a[1], "w") as f:
        json.dump(out, f, separators=(",", ":"))
    print(f"{sum(1 for v in keep.values() if v == 'hle')} HLE + {sum(1 for v in keep.values() if v == 'anchor')} anclas -> {a[1]}")
    if missing:
        print("HLE sin firma en la referencia:", " ".join(missing))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
