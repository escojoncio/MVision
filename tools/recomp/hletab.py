#!/usr/bin/env python3
"""hletab: genera runtime/hle_table.c (nombre -> hle_<nombre>) a partir de las listas runtime/hle*.txt.

  hletab.py runtime/hle*.txt > runtime/hle_table.c      (make -C runtime hle_table)

Cada línea de hle*.txt es "<nombre_neutro> [hash16]": el nombre neutro es el identificador del runtime (hle_<nombre>);
el hash opcional (sha1 del símbolo del mapa de firmas, 16 hex) lo usa tools/sig/hlesigs.py. Nada del juego ni del SDK en claro.
"""
import sys

names = []
for f in sys.argv[1:]:
    for line in open(f):
        t = line.split("#")[0].split()
        n = t[0] if t else ""
        if n and n not in names:
            names.append(n)
names.sort()
print("/* GENERADO por tools/recomp/hletab.py desde runtime/hle*.txt: no editar (make -C runtime hle_table). */")
print('#include "hlebind.h"\n')
for n in names:
    print(f"void hle_{n}(Cpu *c);")
print("\nconst HleEntry hle_table[] = {")
for n in names:
    print(f'    {{"{n}", hle_{n}}},')
print("    {0, 0}\n};")
print(f"const unsigned hle_table_count = {len(names)};")
