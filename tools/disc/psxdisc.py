#!/usr/bin/env python3
"""psxdisc: lector de imágenes BIN/CUE de PlayStation (MODE2/2352).

Solo biblioteca estándar. Nunca escribe datos del juego dentro del repo.

Uso:
  psxdisc.py info    <disco.cue>                 # pista, PVD, ejecutable de arranque
  psxdisc.py ls      <disco.cue>                 # ficheros ISO9660: LBA, tamaño, modo
  psxdisc.py extract <disco.cue> <dir_salida>    # extrae todo (Form 2 en crudo 2336 B/sector)
  psxdisc.py dirtable <disco.cue> <NOMBRE.EXT>    # tabla de archivo DIR (nombre[8], sector)
  psxdisc.py manifest <disco.cue> <salida.json>  # LBA/tamaño/sha256 por fichero (para diffs)
"""
import hashlib
import json
import os
import re
import struct
import sys

RAW = 2352
SYNC = b"\x00" + b"\xff" * 10 + b"\x00"


class Disc:
    def __init__(self, cue_path):
        self.cue_path = cue_path
        base = os.path.dirname(os.path.abspath(cue_path))
        text = open(cue_path, "r", errors="replace").read()
        m = re.search(r'FILE\s+"([^"]+)"', text)
        if not m:
            raise SystemExit("CUE sin FILE")
        self.bin_path = os.path.join(base, m.group(1))
        m = re.search(r"TRACK\s+\d+\s+(\S+)", text)
        self.mode = m.group(1) if m else "?"
        if self.mode not in ("MODE2/2352", "MODE1/2352"):
            raise SystemExit(f"Modo de pista no soportado: {self.mode}")
        self.f = open(self.bin_path, "rb")
        self.sectors = os.path.getsize(self.bin_path) // RAW

    def raw(self, lba):
        self.f.seek(lba * RAW)
        return self.f.read(RAW)

    def sector(self, lba):
        """Devuelve (datos_usuario, es_form2)."""
        s = self.raw(lba)
        if s[:12] != SYNC:
            raise ValueError(f"LBA {lba}: sin sync")
        if self.mode == "MODE1/2352":
            return s[16:16 + 2048], False
        submode = s[18]
        if submode & 0x20:  # Form 2
            return s[24:24 + 2324], True
        return s[24:24 + 2048], False

    def read(self, lba, size):
        out = bytearray()
        while len(out) < size:
            d, f2 = self.sector(lba)
            out += d[:2048] if not f2 else d
            lba += 1
        return bytes(out[:size])

    # --- ISO9660 -----------------------------------------------------------
    def pvd(self):
        d, _ = self.sector(16)
        if d[1:6] != b"CD001":
            raise SystemExit("Sin descriptor ISO9660 en LBA 16")
        return d

    def walk(self):
        pvd = self.pvd()
        root = pvd[156:156 + 34]
        yield from self._dir(struct.unpack_from("<I", root, 2)[0],
                             struct.unpack_from("<I", root, 10)[0], "")

    def _dir(self, lba, size, prefix):
        data = self.read(lba, size)
        i = 0
        while i < len(data):
            ln = data[i]
            if ln == 0:
                i = (i // 2048 + 1) * 2048
                continue
            rec = data[i:i + ln]
            e_lba = struct.unpack_from("<I", rec, 2)[0]
            e_size = struct.unpack_from("<I", rec, 10)[0]
            flags = rec[25]
            nlen = rec[32]
            name = rec[33:33 + nlen]
            i += ln
            if name in (b"\x00", b"\x01"):
                continue
            name = name.decode("ascii", "replace").split(";")[0]
            # Atributos XA (tras el nombre, alineado a par)
            xa_off = 33 + nlen + (1 - nlen % 2)
            xa_attr = 0
            if len(rec) >= xa_off + 14 and rec[xa_off + 6:xa_off + 8] == b"XA":
                xa_attr = struct.unpack_from(">H", rec, xa_off + 4)[0]
            path = f"{prefix}/{name}"
            if flags & 2:
                yield from self._dir(e_lba, e_size, path)
            else:
                form2 = bool(xa_attr & 0x1000) or bool(xa_attr & 0x4000)
                yield {"path": path, "lba": e_lba, "size": e_size,
                       "xa": f"{xa_attr:04x}", "form2": form2}

    def file_bytes(self, ent):
        if not ent["form2"]:
            return self.read(ent["lba"], ent["size"])
        # Form 2 / interleaved (STR, XA): sectores crudos sin cabecera de sync
        n = (ent["size"] + 2047) // 2048
        out = bytearray()
        for k in range(n):
            out += self.raw(ent["lba"] + k)[16:]  # 2336 B: subheader + datos + EDC
        return bytes(out)

    def boot_exe(self):
        files = {e["path"].upper(): e for e in self.walk()}
        cnf = files.get("/SYSTEM.CNF")
        if not cnf:
            return None
        txt = self.file_bytes(cnf).decode("ascii", "replace")
        m = re.search(r"BOOT\s*=\s*cdrom:\\?([^;\s]+)", txt, re.I)
        return m.group(1) if m else txt.strip()


def cmd_info(d):
    pvd = d.pvd()
    print("BIN       :", d.bin_path)
    print("Modo      :", d.mode, "| sectores:", d.sectors)
    print("Volumen   :", pvd[40:72].decode("ascii", "replace").strip())
    print("Sistema   :", pvd[8:40].decode("ascii", "replace").strip())
    print("Arranque  :", d.boot_exe())


def cmd_ls(d):
    for e in sorted(d.walk(), key=lambda e: e["lba"]):
        print(f'{e["lba"]:7d} {e["size"]:11d} {"F2" if e["form2"] else "F1"} {e["xa"]} {e["path"]}')


def cmd_extract(d, out):
    for e in d.walk():
        dst = os.path.join(out, e["path"].lstrip("/"))
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        with open(dst, "wb") as fo:
            fo.write(d.file_bytes(e))
        print("ok", e["path"])


def cmd_dirtable(d, fname):
    ent = next((e for e in d.walk() if e["path"].upper().endswith("/" + fname.upper())), None)
    if not ent:
        raise SystemExit(f"No hay {fname}")
    hdr = d.read(ent["lba"], 2048)
    size = struct.unpack_from("<I", hdr, 0)[0]
    print(f"{fname} LBA {ent['lba']} tamaño tabla {size}")
    for i in range(4, 4 + size, 12):
        name = hdr[i:i + 8].split(b"\0")[0].decode("ascii", "replace")
        off = struct.unpack_from("<i", hdr, i + 8)[0]
        if name:
            print(f"{name:8s} +{off}")


def cmd_manifest(d, out):
    res = []
    for e in d.walk():
        e = dict(e)
        e["sha256"] = hashlib.sha256(d.file_bytes(e)).hexdigest()
        res.append(e)
    json.dump({"boot": d.boot_exe(), "files": res}, open(out, "w"), indent=1)
    print("manifest:", out, len(res), "ficheros")


def main(a):
    if len(a) < 2:
        print(__doc__)
        return 1
    d = Disc(a[1])
    {"info": lambda: cmd_info(d), "ls": lambda: cmd_ls(d),
     "extract": lambda: cmd_extract(d, a[2]), "dirtable": lambda: cmd_dirtable(d, a[2]),
     "manifest": lambda: cmd_manifest(d, a[2])}[a[0]]()
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
