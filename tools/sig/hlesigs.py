#!/usr/bin/env python3
"""hlesigs: filtra sigs_ref.json al subconjunto que necesita el runtime sin código generado.

  hlesigs.py <sigs_ref.json> <salida.json> <hle1.txt> [hle2.txt ...] [--ctx N]

Entrada: firmas del ejecutable de referencia (sigs.py make; solo sha1 enmascarados + tamaño, nunca código).
Salida: las funciones listadas en runtime/hle*.txt (role "hle") más N vecinas por lado en el orden de la
referencia (role "anchor", por defecto 4), que son las anclas con las que hlebind.c valida los emparejamientos
por distancia/orden. Formato y algoritmo de emparejamiento: cabecera de runtime/hlebind.c.
El fichero resultante va dentro del IPA (no se sube al repo): se genera en CI (job `reference`).
"""
import hashlib
import json
import sys


def hname(n):
    """Nombre opaco con el que build.yml guarda las funciones en el artefacto sigs-ref (no revela símbolos externos)."""
    return "h:" + hashlib.sha1(n.encode()).hexdigest()[:16]


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
    names = {}   # nombre neutro -> hash16 del símbolo de la referencia ("" si la línea no lo trae: nombre ya neutro)
    for f in a[2:]:
        for line in open(f):
            t = line.split("#")[0].split()
            if t:
                names[t[0]] = t[1] if len(t) > 1 else ""
    keep = {}
    # Gemelas en la referencia COMPLETA: cuántas funciones comparten el hash completo / de prefijo. El runtime
    # no empareja por hash único funciones con gemelas (un único candidato en el EXE no las distingue).
    fcount, pcount = {}, {}
    for s in sigs:
        fcount[s["full"]] = fcount.get(s["full"], 0) + 1
        pcount[s["prefix"]] = pcount.get(s["prefix"], 0) + 1
    # sigs_ref puede traer los nombres en claro o como "h:<sha1>" (CI): se indexa siempre por el hash opaco.
    byhash = {}
    for i, s in enumerate(sigs):
        byhash[s["name"] if s["name"].startswith("h:") else hname(s["name"])] = i
    idx = {}
    for n, hx in names.items():
        i = byhash.get("h:" + hx) if hx else None
        if i is None:
            i = byhash.get(hname(n))   # nombre ya neutro (capas VR): por si la referencia lo trae igual
        if i is not None:
            idx[n] = i
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
        # Solo las HLE conservan nombre (los de runtime/hle*.txt, ya públicos); las anclas se anonimizan para que ni el IPA ni
        # los artefactos lleven símbolos de la referencia. hlebind.c no usa el nombre de una ancla.
        s["name"] = next(n for n in names if idx.get(n) == i) if keep[i] == "hle" else "a%d" % i
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
