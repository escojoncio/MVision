#!/usr/bin/env python3
"""stages: lector del archivo DIR de stages (tabla + bloques DATACNF).

  stages.py list <DIR>                    # stages y sus ficheros (tags)
  stages.py overlays <DIR> <dir_salida>   # extrae el código de overlay (tag s/b) de cada stage
  stages.py check <DIR>                   # valida que la suma de sectores de cada stage cuadra

Formato (deducido de la decomp y verificado sobre el disco EU):
  DIR    : u32 tamaño_tabla; entradas[12] = nombre[8] + i32 sector (relativo al DIR).
  stage  : u8 version(1) u8 pad i16 sectores_totales; tags[8] = u16 id, u8 modo, u8 ext, i32 tamaño;
           termina en modo 0. Datos tras el sector de cabecera, cada fichero alineado a sector.
           Modo 'c' (cache): grupo empaquetado; el tag con ext 0xFF da el tamaño del grupo.
"""
import os
import struct
import sys

SEC = 2048


def secs(n):
    return (n + SEC - 1) // SEC


def table(d):
    size = struct.unpack_from("<I", d, 0)[0]
    out = []
    for i in range(4, 4 + size, 12):
        name = d[i:i + 8].split(b"\0")[0].decode("ascii", "replace")
        out.append((name, struct.unpack_from("<i", d, i + 8)[0]))
    return out


def stage(d, sector):
    o = sector * SEC
    ver, _, total = struct.unpack_from("<BBh", d, o)
    tags, p = [], o + 4
    while True:
        tid, mode, ext, size = struct.unpack_from("<HBBi", d, p)
        p += 8
        if mode == 0:
            break
        tags.append({"id": tid, "mode": chr(mode), "ext": ext, "size": size})
    # Posiciones de datos
    cur = sector + secs(p - o)
    i = 0
    while i < len(tags):
        t = tags[i]
        if t["mode"] == "c":
            j = i
            while j < len(tags) and tags[j]["mode"] == "c":
                j += 1
            grp = tags[j - 1]["size"]
            for k in range(i, j):
                tags[k]["sector"] = cur
            cur += secs(grp)
            i = j
            continue
        t["sector"] = cur
        cur += secs(t["size"])
        i += 1
    return {"version": ver, "total": total, "tags": tags, "end": cur - sector}


def ext_s(e):
    return chr(e) if 32 < e < 127 else f"0x{e:02x}"


def main(a):
    if len(a) < 2:
        print(__doc__)
        return 1
    d = open(a[1], "rb").read()
    ok = bad = 0
    for name, sec in table(d):
        st = stage(d, sec)
        if a[0] == "list":
            print(f"{name:8s} @{sec:6d} v{st['version']} {st['total']} sect")
            for t in st["tags"]:
                print(f"    {t['id']:04x} {t['mode']} {ext_s(t['ext']):4s} {t['size']:8d}")
        elif a[0] == "check":
            if st["end"] == st["total"]:
                ok += 1
            else:
                bad += 1
                print(f"DESCUADRE {name}: calculado {st['end']} vs cabecera {st['total']}")
        elif a[0] == "overlays":
            os.makedirs(a[2], exist_ok=True)
            for t in st["tags"]:
                if t["mode"] == "s" and t["ext"] == ord("b") and t["size"] > 0:
                    b = d[t["sector"] * SEC:t["sector"] * SEC + t["size"]]
                    open(os.path.join(a[2], f"{name}.bin"), "wb").write(b)
                    print(f"{name}.bin {len(b)}")
    if a[0] == "check":
        print(f"stages correctos {ok}, descuadres {bad}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
