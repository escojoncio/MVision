#!/usr/bin/env python3
"""recomp: recompilador estático MIPS R3000 (original) -> C.

  recomp.py <exe> <salida_dir> [--funcs funcs.txt]... [--code-end HEX] [--names names.json] [--hle hle.txt]...
            [--no-extra] [--poll-entry]
  recomp.py <ov.bin> <salida> --overlay NOMBRE --base HEX --main-gen DIR   (modo overlay, ver más abajo)
  recomp.py --ov-batch <dir_bins> <salida> --base HEX --main-gen DIR

Interrupciones: se emite RT_POLL(c) en cada arista hacia atrás (bucles), en el despacho de tablas de
salto y, con --poll-entry, en la entrada de cada función (sondeo de IRQ pendiente, ver runtime/cpu.h).

--funcs: fichero de direcciones hex (una por línea, # comenta); repetible. Si existe extra_funcs.txt junto
al EXE se lee también (salvo --no-extra): ahí se vuelca missing_funcs.txt del intérprete del runtime.

Salida: funcs_XX.c (una función C por función MIPS), funcs.h, dispatch.c (tabla dirección -> función
y dirección -> nombre). --names: JSON nombre -> {"target": "hex"} (tools/sig/align.py); sus direcciones
se añaden como inicios de función. --hle: lista de nombres cuyo cuerpo se sustituye por hle_<nombre>(c)
(el código original queda como orig_f_<addr> para validación diferencial). Formato de cada línea: "<nombre_neutro> [hash16]";
si hay hash, el nombre del mapa --names cuyo sha1[:16] coincida se traduce al nombre neutro.
Runtime esperado (runtime/cpu.h): struct Cpu { uint32_t r[32], hi, lo; } y helpers de memoria/GTE/COP0.

Detección de funciones: destinos de jal + entrada + lista opcional (firmas). Cada función se
extiende hasta antes del siguiente inicio conocido. Saltos dentro de la función -> goto; fuera -> tail call.
jr de registro distinto de ra -> switch sobre todas las etiquetas de la función (tablas de salto).
Delay slots: la condición se evalúa antes del slot, el slot se ejecuta, luego se salta.
"""
import os
import struct
import sys

# Firma de 8 bytes de la cabecera del formato de ejecutable (hex).
EXE_MAGIC = bytes.fromhex("50532d5820455845")

HDR = 0x800
R = ["zero", "at", "v0", "v1", "a0", "a1", "a2", "a3", "t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7",
     "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7", "t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra"]
FUNCS_PER_FILE = 200
SETJMP = set()  # direcciones de la rutina de guardado de contexto (por nombre neutro)


def rs(w): return (w >> 21) & 31
def rt(w): return (w >> 16) & 31
def rd(w): return (w >> 11) & 31
def sa(w): return (w >> 6) & 31
def imm(w): return w & 0xFFFF
def simm(w): return imm(w) - 0x10000 if imm(w) & 0x8000 else imm(w)


def reg(n):
    return "0u" if n == 0 else f"c->r[{n}]"


def setr(n, expr):
    return "" if n == 0 else f"c->r[{n}] = {expr};"


class Exe:
    def __init__(self, path):
        d = open(path, "rb").read()
        if d[:8] != EXE_MAGIC:
            raise SystemExit("ejecutable no válido")
        self.pc0 = struct.unpack_from("<I", d, 0x10)[0]
        self.base, size = struct.unpack_from("<II", d, 0x18)
        self.text = d[HDR:HDR + size]
        self.end = self.base + len(self.text)

    @classmethod
    def overlay(cls, path, base):
        """Binario de overlay crudo cargado tal cual en `base` (sin cabecera ejecutable)."""
        e = cls.__new__(cls)
        d = open(path, "rb").read()
        e.pc0, e.base, e.is_ov = None, base, True
        e.text = d[:len(d) & ~3]
        e.end = base + len(e.text)
        return e

    is_ov = False

    def word(self, a):
        return struct.unpack_from("<I", self.text, a - self.base)[0]

    def has(self, a):
        return self.base <= a < self.end


def branch_target(a, w):
    return (a + 4 + (simm(w) << 2)) & 0xFFFFFFFF


def is_branch(w):
    op = w >> 26
    if op in (2, 3, 4, 5, 6, 7):
        return True
    if op == 1 and rt(w) in (0, 1, 16, 17):
        return True
    if op == 0 and (w & 63) in (8, 9):
        return True
    return False


def after_end(exe, v):
    """v sigue a un fin de función (jr ra / j + delay) o empieza con prólogo addiu sp,sp,-X."""
    p = exe.word(v - 8) if exe.has(v - 8) else 0
    w = exe.word(v)
    prologue = (w >> 16) == 0x27BD and (w & 0x8000)
    return p == 0x03E00008 or (p >> 26) == 2 or prologue


def find_funcs(exe, extra, weak_out=None):
    """Lista ordenada de inicios. Si weak_out es un set, se rellena con los inicios débiles
    (heurísticos de datos y lui/addiu) que no son también fuertes."""
    starts = ({exe.pc0} if exe.pc0 is not None else set()) | set(extra)
    for a in range(exe.base, exe.end, 4):
        w = exe.word(a)
        if w >> 26 == 3:  # jal
            t = (a & 0xF0000000) | ((w & 0x3FFFFFF) << 2)
            if exe.has(t):
                starts.add(t)
    # Punteros a función en datos: palabra alineada que apunta al código justo tras un `jr ra`+delay.
    full = struct.unpack_from(f"<{len(exe.text) // 4}I", exe.text, 0)
    dptr = set()
    for v in full:
        if exe.code_start <= v < exe.end and v % 4 == 0 and after_end(exe, v):
            dptr.add(v)
    strong = set(starts)
    starts |= dptr
    # Punteros construidos en código: lui rX,hi + addiu/ori rY,rX,lo (p.ej. entradas de hilos y callbacks).
    # El addiu puede ir en el delay slot de un j/jal/jr: se olvida `hi` tras el slot, no antes.
    hi, clear_after = {}, None
    heur = set()
    for a in range(exe.code_start, exe.end, 4):
        w = exe.word(a)
        op = w >> 26
        if op == 15:
            hi[rt(w)] = (imm(w) << 16, a)
        elif op in (9, 13) and rs(w) in hi and a - hi[rs(w)][1] <= 32:
            v = (hi[rs(w)][0] + (simm(w) if op == 9 else imm(w))) & 0xFFFFFFFF
            if exe.code_start <= v < exe.end and v % 4 == 0 and after_end(exe, v):
                heur.add(v)
        if clear_after == a:
            hi.clear()
            clear_after = None
        if op in (2, 3) or (op == 0 and (w & 63) in (8, 9)):
            clear_after = a + 4
    starts.add(exe.code_start)
    base = sorted(s for s in starts if exe.has(s) and s >= exe.code_start)
    # Un inicio heurístico no debe partir una función con tabla jr (sus destinos quedarían fuera del switch).
    jr_ranges = []
    for i, s in enumerate(base):
        e = base[i + 1] if i + 1 < len(base) else exe.end
        if any(Func._is_jr_nonra(exe.word(x)) for x in range(s, e, 4)):
            jr_ranges.append((s, e))
    # Excepción: justo tras `jr ra`+delay es un límite real (la función con tabla acaba ahí).
    # Daño = la parte [s,v) salta (rama o tabla en datos) a [v,e): se descarta; si no, el corte es inocuo.
    data_ptrs = set(struct.unpack_from(f"<{(exe.code_start - exe.base) // 4}I", exe.text, 0))
    if exe.is_ov:  # overlay: tablas de saltos y datos tras el código
        o = exe.end - exe.base
        data_ptrs |= set(struct.unpack_from(f"<{(len(exe.text) - o) // 4}I", exe.text, o))

    def harmful(s, v, e):
        for x in range(s, v, 4):
            w = exe.word(x)
            if is_branch(w) and (w >> 26) in (1, 4, 5, 6, 7) and v <= branch_target(x, w) < e:
                return True
            if (w >> 26) == 2 and v <= ((x & 0xF0000000) | ((w & 0x3FFFFFF) << 2)) < e:
                return True
        # tabla de saltos: el jr queda antes del corte y algún destino en datos cae después
        if not any(Func._is_jr_nonra(exe.word(x)) for x in range(s, v, 4)):
            return False
        return any(v <= p < e for p in data_ptrs)

    bad = {v for v in heur - starts for s, e in jr_ranges if s < v < e and harmful(s, v, e)}
    if bad:
        print("inicios heurísticos descartados (dentro de tabla jr):", " ".join(f"{v:08x}" for v in sorted(bad)))
    res = sorted(set(base) | {v for v in heur - bad if exe.has(v)})
    if weak_out is not None:
        weak_out |= {v for v in res if v not in strong and v != exe.code_start}
    return res


MAX_EXTEND = 64 * 1024


def extend_ends(exe, starts, weak):
    """Fin de cada función = siguiente inicio, ampliado a punto fijo hasta cubrir los destinos de
    rama/j locales que caen en regiones de inicio débil (sin inicio fuerte por medio).
    Devuelve (lista de fines, nº de funciones ampliadas)."""
    import bisect
    ends, n_ext = [], 0
    for i, s in enumerate(starts):
        e = starts[i + 1] if i + 1 < len(starts) else exe.end
        e0, scanned, limit = e, s, s + MAX_EXTEND
        while scanned < e:
            a, scanned_to = scanned, e
            scanned = e
            grow = e
            for x in range(a, scanned_to, 4):
                w = exe.word(x)
                op = w >> 26
                if op == 2:
                    t = (x & 0xF0000000) | ((w & 0x3FFFFFF) << 2)
                elif op in (4, 5, 6, 7) or (op == 1 and rt(w) in (0, 1)):
                    t = branch_target(x, w)
                else:
                    continue
                if t < grow or t >= exe.end or t % 4:
                    continue
                k = bisect.bisect_right(starts, t) - 1  # región que contiene t
                j = bisect.bisect_left(starts, e)
                if starts[k] < e or any(v not in weak for v in starts[j:k + 1]):
                    continue
                ne = starts[k + 1] if k + 1 < len(starts) else exe.end
                if ne > limit:
                    continue
                grow = max(grow, ne)
            e = grow
        if e > e0:
            n_ext += 1
        ends.append(e)
    return ends, n_ext


def detect_code(exe, run=4):
    """Región de código: el tramo más largo sin rachas de `run` palabras no decodificables,
    recortado a [primer prólogo/retorno, último `jr ra` + delay]."""
    best, cur_start, bad, last_ret = (exe.base, exe.base), None, 0, None
    for a in range(exe.base, exe.end + 4, 4):
        w = exe.word(a) if a < exe.end else 0xFFFFFFFF
        ok = a < exe.end and (is_branch(w) or emit_simple(a, w) is not None)
        if ok:
            if cur_start is None:
                cur_start = a
            bad = 0
            if w == 0x03E00008:
                last_ret = a + 8
        else:
            bad += 1
            if bad >= run or a >= exe.end:
                if cur_start is not None and last_ret and last_ret - cur_start > best[1] - best[0]:
                    best = (cur_start, last_ret)
                cur_start, last_ret = None, None
    return best


def cond_expr(a, w):
    op = w >> 26
    s, t = reg(rs(w)), reg(rt(w))
    if op == 4: return f"{s} == {t}"
    if op == 5: return f"{s} != {t}"
    if op == 6: return f"(int32_t){s} <= 0"
    if op == 7: return f"(int32_t){s} > 0"
    if op == 1:
        k = rt(w)
        return f"(int32_t){s} < 0" if k in (0, 16) else f"(int32_t){s} >= 0"
    raise ValueError


def emit_simple(a, w):
    """Instrucción no-salto -> C. Devuelve cadena o None si no soportada."""
    op = w >> 26
    s, t, d = rs(w), rt(w), rd(w)
    S, T = reg(s), reg(t)
    if w == 0:
        return "/* nop */"
    if op == 0:
        f = w & 63
        if f == 0: return setr(d, f"{T} << {sa(w)}")
        if f == 2: return setr(d, f"{T} >> {sa(w)}")
        if f == 3: return setr(d, f"(uint32_t)((int32_t){T} >> {sa(w)})")
        if f == 4: return setr(d, f"{T} << ({S} & 31)")
        if f == 6: return setr(d, f"{T} >> ({S} & 31)")
        if f == 7: return setr(d, f"(uint32_t)((int32_t){T} >> ({S} & 31))")
        if f == 12: return f"rt_syscall(c, 0x{a:08x});"
        if f == 13: return f"rt_break(c, 0x{a:08x});"
        if f == 16: return setr(d, "c->hi")
        if f == 17: return f"c->hi = {S};"
        if f == 18: return setr(d, "c->lo")
        if f == 19: return f"c->lo = {S};"
        if f == 24: return f"{{ int64_t m = (int64_t)(int32_t){S} * (int32_t){T}; c->lo = (uint32_t)m; c->hi = (uint32_t)(m >> 32); }}"
        if f == 25: return f"{{ uint64_t m = (uint64_t){S} * {T}; c->lo = (uint32_t)m; c->hi = (uint32_t)(m >> 32); }}"
        if f == 26: return f"rt_div(c, {S}, {T});"
        if f == 27: return f"rt_divu(c, {S}, {T});"
        if f in (32, 33): return setr(d, f"{S} + {T}")
        if f in (34, 35): return setr(d, f"{S} - {T}")
        if f == 36: return setr(d, f"{S} & {T}")
        if f == 37: return setr(d, f"{S} | {T}")
        if f == 38: return setr(d, f"{S} ^ {T}")
        if f == 39: return setr(d, f"~({S} | {T})")
        if f == 42: return setr(d, f"(int32_t){S} < (int32_t){T}")
        if f == 43: return setr(d, f"{S} < {T}")
        return None
    I, U = simm(w), imm(w)
    ea = f"{S} + {I}"
    if op in (8, 9): return setr(t, f"{S} + {I}")
    if op == 10: return setr(t, f"(int32_t){S} < {I}")
    if op == 11: return setr(t, f"{S} < (uint32_t){I}")
    if op == 12: return setr(t, f"{S} & 0x{U:x}u")
    if op == 13: return setr(t, f"{S} | 0x{U:x}u")
    if op == 14: return setr(t, f"{S} ^ 0x{U:x}u")
    if op == 15: return setr(t, f"0x{U << 16:08x}u")
    if op == 32: return setr(t, f"(uint32_t)(int8_t)mem_r8(c, {ea})") or f"(void)mem_r8(c, {ea});"
    if op == 33: return setr(t, f"(uint32_t)(int16_t)mem_r16(c, {ea})") or f"(void)mem_r16(c, {ea});"
    if op == 34: return setr(t, f"mem_lwl(c, {ea}, {T})")
    if op == 35: return setr(t, f"mem_r32(c, {ea})") or f"(void)mem_r32(c, {ea});"
    if op == 36: return setr(t, f"mem_r8(c, {ea})") or f"(void)mem_r8(c, {ea});"
    if op == 37: return setr(t, f"mem_r16(c, {ea})") or f"(void)mem_r16(c, {ea});"
    if op == 38: return setr(t, f"mem_lwr(c, {ea}, {T})")
    if op == 40: return f"mem_w8(c, {ea}, (uint8_t){T});"
    if op == 41: return f"mem_w16(c, {ea}, (uint16_t){T});"
    if op == 42: return f"mem_swl(c, {ea}, {T});"
    if op == 43: return f"mem_w32(c, {ea}, {T});"
    if op == 46: return f"mem_swr(c, {ea}, {T});"
    if op == 50: return f"gte_lwc2(c, {t}, mem_r32(c, {ea}));"
    if op == 58: return f"mem_w32(c, {ea}, gte_swc2(c, {t}));"
    if op == 16:  # COP0
        k = s
        if k == 0: return setr(t, f"cop0_read(c, {d})")
        if k == 4: return f"cop0_write(c, {d}, {T});"
        if k == 16 and (w & 63) == 16: return "cop0_rfe(c);"
        return None
    if op == 18:  # COP2 (GTE)
        k = s
        if k & 16: return f"gte_cmd(c, 0x{w & 0x1FFFFFF:07x});"
        if k == 0: return setr(t, f"gte_mfc2(c, {d})")
        if k == 2: return setr(t, f"gte_cfc2(c, {d})")
        if k == 4: return f"gte_mtc2(c, {d}, {T});"
        if k == 6: return f"gte_ctc2(c, {d}, {T});"
        return None
    return None


class Func:
    def __init__(self, exe, start, end, known, pfx="f_", ext=()):
        self.exe, self.start, self.end, self.known = exe, start, end, known
        self.pfx, self.ext = pfx, ext   # overlay: prefijo propio y conjunto de inicios del ejecutable principal
        self.labels = set()
        self.unsupported = 0
        for a in range(start, end, 4):
            w = exe.word(a)
            op = w >> 26
            if op in (1, 4, 5, 6, 7) and is_branch(w):
                t = branch_target(a, w)
                if start <= t < end:
                    self.labels.add(t)
            elif op == 2:
                t = (a & 0xF0000000) | ((w & 0x3FFFFFF) << 2)
                if start <= t < end:
                    self.labels.add(t)
        self.has_jr_table = any(self._is_jr_nonra(exe.word(a)) for a in range(start, end, 4))
        if self.has_jr_table:
            self.labels |= set(range(start, end, 4))

    @staticmethod
    def _is_jr_nonra(w):
        return w >> 26 == 0 and (w & 63) == 8 and rs(w) != 31

    def sym(self, t):
        """Símbolo C de la función que empieza en t (propia o del ejecutable principal) o None."""
        if t in self.known: return f"{self.pfx}{t:08x}"
        if t in self.ext: return f"f_{t:08x}"
        return None

    def goto_or_tail(self, t, a=None):
        if self.start <= t < self.end:
            # arista hacia atrás (bucle): punto seguro de entrega de interrupciones
            if a is not None and t <= a:
                return f"RT_POLL(c); goto L_{t:08x};"
            return f"goto L_{t:08x};"
        if self.sym(t):
            return f"{self.sym(t)}(c); return;"
        return f"rt_jump(c, 0x{t:08x}u); return;"

    def call(self, t, ret=None):
        """Llamada a t. El registro de enlace se fija aparte (ver link()): en el hardware se escribe
        ANTES de ejecutar el delay slot, así que el slot lo ve (y si lo escribe, gana el slot).
        Si se pasa ret (uso suelto, sin slot) se antepone la escritura de ra."""
        pre = f"c->r[31] = 0x{ret:08x}u; " if ret is not None else ""
        if t in SETJMP:
            # setjmp del juego: punto de reanudación en el marco C actual (ver runtime/rt.c rt_sj_*).
            return (f"{pre}if (setjmp(*rt_sj_slot(c, c->r[4]))) rt_sj_resumed(c); "
                    f"else {self.sym(t) or 'f_%08x' % t}(c);")
        if self.sym(t):
            return f"{pre}{self.sym(t)}(c);"
        return f"{pre}rt_call(c, 0x{t:08x}u);"

    @staticmethod
    def link(n, ret):
        """Escritura del registro de enlace (ra o rd de jalr) previa al delay slot."""
        return setr(n, f"0x{ret:08x}u")

    def simple(self, a):
        w = self.exe.word(a)
        out = emit_simple(a, w)
        if out is None:
            self.unsupported += 1
            return f"rt_unimpl(c, 0x{a:08x}u, 0x{w:08x}u);"
        return out

    def emit(self, name=None, hle=False):
        fname = f"orig_f_{self.start:08x}" if hle else f"{self.pfx}{self.start:08x}"
        L = []
        if hle:
            L += [f"void f_{self.start:08x}(Cpu *c) {{ RT_ENTER(0x{self.start:08x}u); hle_{name}(c); }}", ""]
        L += [f"void {fname}(Cpu *c) {{" + (f" /* {name} */" if name else ""),
              "    uint32_t tgt; (void)tgt;"]
        if not hle:
            L.append(f"    RT_ENTER(0x{self.start:08x}u);")
            if POLL_ENTRY:
                L.append("    RT_POLL(c);")
        a = self.start
        while a < self.end:
            w = self.exe.word(a)
            lab = f"L_{a:08x}: " if a in self.labels else ""
            if is_branch(w) and a + 4 < self.end + 4:
                ds = self.simple(a + 4) if self.exe.has(a + 4) else ""
                op = w >> 26
                f = w & 63
                body = []
                if op == 2:
                    t = (a & 0xF0000000) | ((w & 0x3FFFFFF) << 2)
                    body = [ds, self.goto_or_tail(t, a)]
                elif op == 3:
                    t = (a & 0xF0000000) | ((w & 0x3FFFFFF) << 2)
                    body = [self.link(31, a + 8), ds, self.call(t)]
                elif op == 0 and f == 8:  # jr
                    if rs(w) == 31:
                        body = [f"tgt = {reg(31)};", ds, "return;"]
                    elif self.has_jr_table:
                        body = [f"tgt = {reg(rs(w))};", ds, "goto JR_DISPATCH;"]
                    else:
                        body = [f"tgt = {reg(rs(w))};", ds, "rt_jump(c, tgt); return;"]
                elif op == 0 and f == 9:  # jalr
                    body = [f"tgt = {reg(rs(w))};", self.link(rd(w), a + 8), ds, "rt_call(c, tgt);"]
                else:
                    t = branch_target(a, w)
                    link = op == 1 and rt(w) in (16, 17)
                    pre = [f"{{ int cond = {cond_expr(a, w)};"]
                    if link:
                        pre.append(f"c->r[31] = 0x{a + 8:08x}u;")
                    tk = self.call(t) if link else self.goto_or_tail(t, a)
                    body = pre + [ds, f"if (cond) {{ {tk} }} }}"]
                L.append(f"    {lab}" + " ".join(x for x in body if x))
                # El delay slot también puede ser destino de otro salto (raro): etiqueta propia
                if a + 4 in self.labels:
                    L.append(f"    goto S_{a:08x}; L_{a + 4:08x}: {ds} S_{a:08x}:;")
                a += 8
                continue
            L.append(f"    {lab}{self.simple(a)}")
            a += 4
        L.append(f"    {self.sym(self.end)}(c); return; /* caída a la siguiente función */"
                 if self.sym(self.end) else "    rt_fallthrough(c, 0x%08xu); return;" % self.end)
        if self.has_jr_table:
            L.append("JR_DISPATCH:")
            L.append("    RT_POLL(c);")
            L.append("    switch (tgt) {")
            for x in sorted(self.labels):
                L.append(f"    case 0x{x:08x}u: goto L_{x:08x};")
            L.append("    default: rt_jump(c, tgt); return;")
            L.append("    }")
        L.append("}")
        return "\n".join(L)



# ---------------------------------------------------------------------------------------------
# Modo overlay (código de zona cargado en RAM en una base común; ver bitácora §15).
#   recomp.py <ov.bin> <salida> --overlay NOMBRE --base HEX --main-gen DIR_GEN_PRINCIPAL
#   recomp.py --ov-batch <dir_bins> <salida> --base HEX --main-gen DIR_GEN_PRINCIPAL
# Genera ov_<nombre>.c (funciones ov_<nombre>_f_XXXXXXXX + tabla ov_<nombre>_table) y, en lote, el
# índice ov_index.c. Dos overlays con el mismo código (mismo hash) comparten tabla.
# ---------------------------------------------------------------------------------------------
def fnv1a64(b):
    h = 0xcbf29ce484222325
    for x in b:
        h = ((h ^ x) * 0x100000001b3) & 0xFFFFFFFFFFFFFFFF
    return h


def load_main_gen(gen):
    """Inicios de función y dirección de la rutina de salto no local del ejecutable principal ya recompilado."""
    import re
    starts = {int(m, 16) for m in re.findall(r"void f_([0-9a-f]{8})\(Cpu", open(os.path.join(gen, "funcs.h")).read())}
    disp = open(os.path.join(gen, "dispatch.c")).read()
    sj = {int(m, 16) for m in re.findall(r'\{0x([0-9a-f]{8})u, "jmp_save"\}', disp)}
    return starts, sj


def ov_region(path, base):
    """Región de código del overlay, afinada: el inicio sube hasta la primera función real (los datos
    de cabecera que se decodifican como instrucciones quedan fuera del hash). Devuelve
    (exe, inicio, fin, hash, inicios, fines)."""
    exe = Exe.overlay(path, base)
    cs, ce = detect_code(exe)
    exe.end, exe.code_start = ce, cs
    starts = find_funcs(exe, [], set())
    firsts = [x for x in starts if x != cs]
    if firsts and min(firsts) > cs:
        exe.code_start = cs = min(firsts)
    weak = set()
    starts = find_funcs(exe, [], weak)
    ends, _ = extend_ends(exe, starts, weak)
    return exe, cs, ce, fnv1a64(exe.text[cs - base:ce - base]), starts, ends


def ov_ident(name):
    return "".join(ch if ch.isalnum() else "_" for ch in name)


def recompile_overlay(exe, name, outdir, main_starts, starts, ends):
    """Escribe ov_<name>.c. Devuelve (nº de funciones, nº de instrucciones no soportadas)."""
    ident = ov_ident(name)
    pfx = f"ov_{ident}_f_"
    known = set(starts)
    funcs = [Func(exe, s, ends[i], known, pfx, main_starts) for i, s in enumerate(starts)]
    with open(os.path.join(outdir, f"ov_{ident}.c"), "w") as f:
        f.write('#include "funcs.h"\n\n')
        for fn in funcs:
            f.write(f"static void {pfx}{fn.start:08x}(Cpu *c);\n")
        f.write("\n")
        for fn in funcs:
            text = fn.emit()
            f.write(text.replace(f"void {pfx}{fn.start:08x}(", f"static void {pfx}{fn.start:08x}(", 1) + "\n\n")
        f.write(f"const FuncEntry ov_{ident}_table[] = {{\n")
        for fn in funcs:
            f.write(f"    {{0x{fn.start:08x}u, {pfx}{fn.start:08x}}},\n")
        f.write("};\n")
    return len(funcs), sum(fn.unsupported for fn in funcs)


def overlay_main(a):
    base = int(a[a.index("--base") + 1], 16)
    main_starts, sj = load_main_gen(a[a.index("--main-gen") + 1])
    SETJMP.update(sj)
    if "--ov-batch" in a:
        i = a.index("--ov-batch")
        bindir, outdir = a[i + 1], a[i + 2]
        os.makedirs(outdir, exist_ok=True)
        bins = sorted(x for x in os.listdir(bindir) if x.endswith(".bin") and os.path.getsize(os.path.join(bindir, x)) >= 64)
        done, rows, words = {}, [], {}   # (hash, off, len) -> nombre con tabla
        for fn in bins:
            name = fn[:-4]
            exe, cs, ce, h, st, en = ov_region(os.path.join(bindir, fn), base)
            key = (h, cs - base, ce - cs)
            if key not in done:
                n, un = recompile_overlay(exe, name, outdir, main_starts, st, en)
                done[key] = (name, n)
                print(f"{name}: código 0x{cs:08x}-0x{ce:08x} hash {h:016x} funciones {n} no soportadas {un}")
            else:
                print(f"{name}: igual que {done[key][0]}")
            words[name] = exe.text
            rows.append((name, key, done[key]))
        with open(os.path.join(outdir, "ov_index.c"), "w") as f:
            f.write('#include "cpu.h"\n\n')
            for tn in sorted({d[0] for _, _, d in rows}):
                f.write(f"extern const FuncEntry ov_{ov_ident(tn)}_table[];\n")
            f.write("\nconst OvDesc rt_ov_index[] = {\n")
            for name, (h, off, ln), (tn, n) in rows:
                w0 = struct.unpack_from("<I", words[name], off)[0]
                f.write(f'    {{"{name}", 0x{base:08x}u, 0x{off:x}u, 0x{ln:x}u, 0x{w0:08x}u, 0x{h:016x}ull, ov_{ov_ident(tn)}_table, {n}u}},\n')
            f.write("    {0, 0, 0, 0, 0, 0, 0, 0}\n};\n")
            f.write(f"const unsigned rt_ov_count = {len(rows)};\n")
        print(f"overlays {len(rows)}, códigos distintos {len(done)}")
        return 0
    path, outdir = a[0], a[1]
    name = a[a.index("--overlay") + 1]
    os.makedirs(outdir, exist_ok=True)
    exe, cs, ce, h, st, en = ov_region(path, base)
    n, un = recompile_overlay(exe, name, outdir, main_starts, st, en)
    print(f"{name}: código 0x{cs:08x}-0x{ce:08x} hash {h:016x} funciones {n} no soportadas {un}")
    return 0


POLL_ENTRY = False  # --poll-entry: sondeo también en la entrada de cada función


def main(a):
    global POLL_ENTRY
    POLL_ENTRY = "--poll-entry" in a
    if "--overlay" in a or "--ov-batch" in a:
        return overlay_main(a)
    if len(a) < 2:
        print(__doc__)
        return 1
    exe = Exe(a[0])
    out = a[1]
    extra = []
    files = [a[i + 1] for i, x in enumerate(a) if x == "--funcs"]  # --funcs repetible
    # Fichero opcional junto al EXE (local): direcciones registradas por el intérprete del runtime.
    auto = os.path.join(os.path.dirname(os.path.abspath(a[0])), "extra_funcs.txt")
    if "--no-extra" not in a and os.path.exists(auto):
        files.append(auto)
    for fn in files:
        for l in open(fn):
            l = l.split("#")[0].strip()
            if l:
                extra.append(int(l, 16))
    code_start, code_end = detect_code(exe)
    if "--code-end" in a:
        code_end = int(a[a.index("--code-end") + 1], 16)
    exe.end, exe.code_start = code_end, code_start
    names = {}
    if "--names" in a:
        import json
        sure = []
        for n, v in json.load(open(a[a.index("--names") + 1])).items():
            t = int(v["target"], 16)
            if code_start <= t < code_end:
                names.setdefault(t, n)
                # solo los métodos fiables crean inicios de función; el resto solo nombra inicios ya detectados
                if v.get("how") in ("exact", "prefijo"):
                    sure.append(t)
        extra += sure
    hle = set()
    alias = {}   # sha1[:16] del símbolo del mapa -> nombre neutro
    for i, x in enumerate(a):  # --hle repetible: un fichero por subsistema (runtime/hle*.txt)
        if x == "--hle":
            for l in open(a[i + 1]):
                t = l.split("#")[0].split()
                if t:
                    hle.add(t[0])
                    if len(t) > 1:
                        alias[t[1]] = t[0]
    if alias:
        import hashlib
        names = {t: alias.get(hashlib.sha1(n.encode()).hexdigest()[:16], n) for t, n in names.items()}
    SETJMP.update(t for t, n in names.items() if n == "jmp_save")
    weak = set()
    starts = find_funcs(exe, extra, weak)
    # Funciones sustituidas por HLE o setjmp deben conservar su entrada: nunca se absorben en otra.
    weak -= {t for t, n in names.items() if n in hle or n == "jmp_save"}
    ends, n_ext = extend_ends(exe, starts, weak)
    print(f"funciones ampliadas por ramas a regiones débiles: {n_ext}")
    known = set(starts)
    os.makedirs(out, exist_ok=True)
    funcs = []
    for i, s in enumerate(starts):
        funcs.append(Func(exe, s, ends[i], known))
    with open(os.path.join(out, "funcs.h"), "w") as f:
        f.write('#pragma once\n#include <setjmp.h>\n#include "cpu.h"\n'
                'jmp_buf *rt_sj_slot(Cpu *c, uint32_t buf);\nvoid rt_sj_resumed(Cpu *c);\n')
        for fn in funcs:
            f.write(f"void f_{fn.start:08x}(Cpu *c);\n")
            n = names.get(fn.start)
            if n in hle:
                f.write(f"void orig_f_{fn.start:08x}(Cpu *c);\nvoid hle_{n}(Cpu *c);\n")
    unsup = 0
    for k in range(0, len(funcs), FUNCS_PER_FILE):
        with open(os.path.join(out, f"funcs_{k // FUNCS_PER_FILE:02d}.c"), "w") as f:
            f.write('#include "funcs.h"\n\n')
            for fn in funcs[k:k + FUNCS_PER_FILE]:
                n = names.get(fn.start)
                f.write(fn.emit(n, n in hle) + "\n\n")
                unsup += fn.unsupported
    with open(os.path.join(out, "dispatch.c"), "w") as f:
        f.write('#include "funcs.h"\n\nconst FuncEntry rt_func_table[] = {\n')
        for fn in funcs:
            f.write(f"    {{0x{fn.start:08x}u, f_{fn.start:08x}}},\n")
        f.write(f"}};\nconst unsigned rt_func_count = {len(funcs)};\n")
        f.write(f"const uint32_t rt_entry = 0x{exe.pc0:08x}u;\n")
        # Código original de las funciones sustituidas por HLE: el HLE puede delegar en él (rt_orig).
        ho = sorted((names[fn.start], fn.start) for fn in funcs if names.get(fn.start) in hle)
        f.write("const HleOrig rt_hle_orig[] = {\n")
        for n, t in ho:
            f.write(f'    {{"{n}", orig_f_{t:08x}}},\n')
        f.write("    {0, 0}\n};\n")
        f.write(f"const unsigned rt_hle_orig_count = {len(ho)};\n")
        nl = sorted((t, n) for t, n in names.items() if t in known)
        f.write("const FuncName rt_func_names[] = {\n")
        for t, n in nl:
            f.write(f'    {{0x{t:08x}u, "{n}"}},\n')
        f.write("    {0xFFFFFFFFu, \"\"}\n};\n")
        f.write(f"const unsigned rt_func_name_count = {len(nl)};\n")
    missing = hle - {names.get(fn.start) for fn in funcs}
    if missing:
        print("HLE sin dirección:", " ".join(sorted(missing)))
    print(f"código 0x{code_start:08x}-0x{code_end:08x} | funciones {len(funcs)} | jr-tabla {sum(f.has_jr_table for f in funcs)} | no soportadas {unsup}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
