#!/usr/bin/env python3
"""Fusiona el mapa de nombres base (JSON de tools/sig, local) con vr_names.json: los nombres neutros de la capa
de cámara sustituyen a cualquier nombre que el mapa base tuviera en la misma dirección (el recompilador conserva
el primer nombre de cada dirección). Uso: vr_names.py BASE.json SALIDA.json [vr_names.json]"""
import json
import os
import sys

def main(a):
    if len(a) < 2:
        print(__doc__); return 1
    base = json.load(open(a[0]))
    extra = json.load(open(a[2] if len(a) > 2 else os.path.join(os.path.dirname(os.path.abspath(__file__)), "vr_names.json")))
    taken = {v["target"].lower() for v in extra.values()}
    out = {k: v for k, v in base.items() if v["target"].lower() not in taken}
    for k, v in extra.items():
        out[k] = {"target": v["target"], "how": v.get("how", "exact")}
    json.dump(out, open(a[1], "w"), indent=0)
    print(f"{len(out)} nombres ({sum(1 for v in base.values() if v['target'].lower() in taken)} sustituidos)")
    return 0

if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
