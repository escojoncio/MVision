#!/usr/bin/env python3
"""Pruebas del recompilador: semántica del registro de enlace respecto al delay slot.

En el hardware jal/jalr/bltzal/bgezal escriben el enlace ANTES de ejecutar el slot: el slot ve el
valor nuevo y, si escribe ese mismo registro, gana el slot. Se ensamblan a mano casos sintéticos,
se recompilan con recomp.Func y se ejecutan compilando el C emitido contra un Cpu mínimo.
Uso: python3 tools/recomp/test_recomp.py   (necesita cc en el PATH)
"""
import os
import struct
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import recomp as R

BASE = 0x80010000
CALLEE = BASE + 0x100          # función "conocida": c->r[10] = ra visto; c->r[11]++
NOP, JR_RA = 0, 0x03E00008
SENT = 0xDEAD0001


def jal(t): return 0x0C000000 | ((t >> 2) & 0x3FFFFFF)
def jalr(rs, rd): return (rs << 21) | (rd << 11) | 9
def addu(rd, rs, rt): return (rs << 21) | (rt << 16) | (rd << 11) | 0x21
def addiu(rt, rs, imm): return 0x24000000 | (rs << 21) | (rt << 16) | (imm & 0xFFFF)
def bxal(a, rs, link_rt, t):    # bltzal (rt=16) / bgezal (rt=17) en la dirección a hacia t
    return (1 << 26) | (rs << 21) | (link_rt << 16) | (((t - (a + 4)) >> 2) & 0xFFFF)


def at(k): return BASE + 0x1000 * (k + 1)   # cada caso en su propia función


# Cada caso: nombre, constructor(a) -> palabras, estado inicial {reg: val}, comprobaciones {reg: val(a)}
# a = dirección del salto; el enlace esperado es a + 8.
CASES = [
    ("jal_slot_lee_ra", lambda a: [jal(CALLEE), addu(5, 31, 0)], {31: SENT},
     lambda a: {5: a + 8, 10: a + 8, 11: 1}),
    ("jal_slot_escribe_ra", lambda a: [jal(CALLEE), addiu(31, 0, 0x1234)], {31: SENT},
     lambda a: {10: 0x1234, 11: 1}),
    ("jalr_rd20_slot_lee_rd", lambda a: [jalr(8, 20), addu(5, 20, 0)], {8: CALLEE, 31: SENT},
     lambda a: {5: a + 8, 20: a + 8, 10: SENT, 11: 1}),
    ("jalr_rd31_slot_escribe_ra", lambda a: [jalr(8, 31), addiu(31, 0, 0x4321)], {8: CALLEE, 31: SENT},
     lambda a: {10: 0x4321, 11: 1}),
    ("jalr_rd_igual_rs", lambda a: [jalr(8, 8), addu(5, 8, 0)], {8: CALLEE},
     lambda a: {5: a + 8, 8: a + 8, 11: 1}),
    ("bltzal_no_tomado", lambda a: [bxal(a, 4, 16, CALLEE), addu(5, 31, 0)], {4: 5, 31: SENT},
     lambda a: {5: a + 8, 31: a + 8, 11: 0}),
    ("bltzal_tomado", lambda a: [bxal(a, 4, 16, CALLEE), addu(5, 31, 0)], {4: 0xFFFFFFFF, 31: SENT},
     lambda a: {5: a + 8, 10: a + 8, 11: 1}),
    ("bgezal_no_tomado", lambda a: [bxal(a, 4, 17, CALLEE), addu(5, 31, 0)], {4: 0xFFFFFFFF, 31: SENT},
     lambda a: {5: a + 8, 31: a + 8, 11: 0}),
    ("bgezal_slot_escribe_ra_tomado", lambda a: [bxal(a, 4, 17, CALLEE), addiu(31, 0, 0x77)],
     {4: 1, 31: SENT}, lambda a: {10: 0x77, 11: 1}),
    # el slot cambia r4 pero la condición ya estaba evaluada antes del slot
    ("bltzal_cond_antes_del_slot", lambda a: [bxal(a, 4, 16, CALLEE), addiu(4, 0, 5)],
     {4: 0xFFFFFFFF, 31: SENT}, lambda a: {10: a + 8, 11: 1}),
]


def build():
    size = 0x1000 * (len(CASES) + 2)
    text = bytearray(size)
    exe = R.Exe.__new__(R.Exe)
    exe.pc0, exe.base, exe.text, exe.end = None, BASE, text, BASE + size
    chunks, names = [], []
    for i, (name, mk, init, exp) in enumerate(CASES):
        a = at(i)
        words = mk(a) + [JR_RA, NOP]
        for j, w in enumerate(words):
            struct.pack_into("<I", text, a - BASE + 4 * j, w)
        chunks.append(R.Func(exe, a, a + 4 * len(words), {CALLEE}).emit())
        names.append(f"f_{a:08x}")
    return chunks, names


def harness(chunks, names):
    L = ["#include <stdint.h>", "#include <stdio.h>", "#include <setjmp.h>",
         "typedef struct { uint32_t r[32]; } Cpu;",
         "#define RT_ENTER(x) ((void)0)", "#define RT_POLL(c) ((void)0)",
         "static int bad;",
         "void f_%08x(Cpu *c) { c->r[10] = c->r[31]; c->r[11]++; }" % CALLEE,
         "static void rt_call(Cpu *c, uint32_t t) { if (t == 0x%08xu) f_%08x(c); else bad++; }" % (CALLEE, CALLEE),
         "static void rt_jump(Cpu *c, uint32_t t) { (void)c; (void)t; bad++; }",
         "static void rt_fallthrough(Cpu *c, uint32_t t) { (void)c; (void)t; bad++; }",
         "static void rt_unimpl(Cpu *c, uint32_t a, uint32_t w) { (void)c; (void)a; (void)w; bad++; }"]
    L += chunks
    L.append("int main(void) { int fails = 0; Cpu c;")
    for i, ((name, mk, init, exp), fn) in enumerate(zip(CASES, names)):
        L.append("  { __builtin_memset(&c, 0, sizeof c); bad = 0;")
        for r, v in init.items():
            L.append(f"    c.r[{r}] = 0x{v:08x}u;")
        L.append(f"    {fn}(&c);")
        for r, v in exp(at(i)).items():
            L.append(f'    if (c.r[{r}] != 0x{v:08x}u) {{ printf("FALLO {name}: r{r} = %08x, esperado {v:08x}\\n", c.r[{r}]); fails++; }}')
        L.append(f'    if (bad) {{ printf("FALLO {name}: ruta inesperada\\n"); fails++; }}')
        L.append(f'    else printf("ok {name}\\n"); }}')
    L.append("  return fails != 0; }")
    return "\n".join(L)


def main():
    chunks, names = build()
    src = harness(chunks, names)
    with tempfile.TemporaryDirectory() as d:
        c, exe = os.path.join(d, "t.c"), os.path.join(d, "t")
        open(c, "w").write(src)
        r = subprocess.run([os.environ.get("CC", "cc"), "-O1", "-w", "-o", exe, c])
        if r.returncode:
            sys.exit("el C emitido no compila")
        sys.exit(subprocess.run([exe]).returncode)


if __name__ == "__main__":
    main()
