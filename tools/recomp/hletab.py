#!/usr/bin/env python3
"""hletab: genera runtime/hle_table.c (nombre -> hle_<nombre>) a partir de las listas runtime/hle*.txt.

  hletab.py runtime/hle*.txt > runtime/hle_table.c      (make -C runtime hle_table)

Solo contiene nombres de funciones de librería del SDK y símbolos de nuestro runtime: nada del juego.
"""
import sys

names = []
for f in sys.argv[1:]:
    for line in open(f):
        n = line.split("#")[0].strip()
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
