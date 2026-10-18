#!/usr/bin/env python3
"""extract: vuelca los modelos 3D del motor a OBJ+MTL+PNG y genera hojas de contacto (solo stdlib).

  extract.py <archivo_stages> <dir_salida> [--dict nombres.txt] [--resident <stage>] [--only-resident] [--size 96] [--cols 16]

Salida (fuera del repo, nunca subir): <out>/obj/<stage>_<id>.obj|mtl, <out>/tex/<id>.png,
<out>/sheet_NN.png (hojas de contacto), <out>/index.txt (stage, id, objetos, caras, texturas, componentes).
--dict: fichero de texto (uno por línea) con nombres candidatos; los que coincidan por id16 con un id
de modelo se anotan en el índice (el diccionario vive fuera del repo).
--resident: nombre del stage con recursos compartidos (texturas/modelos comunes); se pasa por argumento.
Orientación del OBJ: la del motor (y arriba); coordenadas en metros (unidad original / 1000).
"""
import hashlib
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "stage"))
import mdlfmt as M  # noqa: E402
import raster as R  # noqa: E402
import stages  # noqa: E402


def textures(d, tb, name, init_cache):
    """id16 -> (w,h,img,id24) con las texturas del stage más las residentes."""
    out = dict(init_cache)
    c, bl = M.stage_blocks(d, tb, stages, name)
    for i, x, b in bl + c:
        if x == "p" and b:
            r = M.decode_img(b)
            if r:
                out[i & 0xFFFF] = r
    return out


def components(m):
    """Nº de componentes conexas por vértices compartidos (pistas para piezas móviles)."""
    n = 0
    for o in m["objs"]:
        par = list(range(len(o["verts"])))

        def f(a):
            while par[a] != a:
                par[a] = par[par[a]]
                a = par[a]
            return a
        used = set()
        for fc in o["faces"]:
            vi = fc[0]
            used.update(vi)
            for k in vi[1:]:
                par[f(k)] = f(vi[0])
        n += len({f(v) for v in used if v < len(par)})
    return n


def write_obj(path_base, name, tris, texmap):
    mats = sorted({t[9] for t in tris})
    with open(path_base + ".mtl", "w") as f:
        for t in mats:
            f.write(f"newmtl t{t:04x}\nKd 1 1 1\n")
            if t in texmap:
                f.write(f"map_Kd ../tex/{t:04x}.png\n")
    with open(path_base + ".obj", "w") as f:
        f.write(f"mtllib {os.path.basename(path_base)}.mtl\no {name}\n")
        vi = 1
        cur = None
        for t in sorted(tris, key=lambda t: t[9]):
            if t[9] != cur:
                cur = t[9]
                f.write(f"usemtl t{cur:04x}\n")
            for p in t[:3]:
                f.write(f"v {p[0]/1000:.5f} {p[1]/1000:.5f} {p[2]/1000:.5f}\n")
            for u in t[3:6]:
                f.write(f"vt {u[0]:.5f} {1 - u[1]:.5f}\n")
            f.write(f"vn {t[7][0]:.4f} {t[7][1]:.4f} {t[7][2]:.4f}\n")
            f.write(f"f {vi}/{vi}/{vi//3+1} {vi+1}/{vi+1}/{vi//3+1} {vi+2}/{vi+2}/{vi//3+1}\n")
            vi += 3


def main(a):
    if len(a) < 2:
        print(__doc__)
        return 1
    d = open(a[0], "rb").read()
    out = a[1]
    opt = lambda k, dv=None: a[a.index(k) + 1] if k in a else dv  # noqa: E731
    size, cols = int(opt("--size", 96)), int(opt("--cols", 16))
    names = {}
    if opt("--dict"):
        for ln in open(opt("--dict"), encoding="utf8"):
            ln = ln.strip()
            if ln:
                names[M.id16(ln)] = ln
    for sd in ("obj", "tex"):
        os.makedirs(os.path.join(out, sd), exist_ok=True)
    tb = dict(stages.table(d))
    # texturas residentes
    ic = {}
    res = opt("--resident")
    if res in tb:
        c, bl = M.stage_blocks(d, tb, stages, res)
        for i, x, b in bl + c:
            if x == "p" and b:
                r = M.decode_img(b)
                if r:
                    ic[i & 0xFFFF] = r
    seen, imgs, idx, texdone = {}, [], [], set()
    for sn in ([res] if "--only-resident" in a else list(tb)):
        tx = textures(d, tb, sn, ic)
        c, bl = M.stage_blocks(d, tb, stages, sn)
        for i, x, b in c + bl:
            if x != "k":
                continue
            m = M.parse_mdl(b)
            if not m:
                continue
            h = hashlib.sha1(b).hexdigest()
            if h in seen:
                seen[h][1].append(sn)
                continue
            texmap = {}
            tris = M.build_tris(m, lambda t: (tx[t][0], tx[t][1], R.tex_rgba(tx[t])) if t in tx else None)
            for t in {t[9] for t in tris}:
                if t in tx:
                    texmap[t] = 1
                    if (t, id(tx[t])) not in texdone:
                        texdone.add((t, id(tx[t])))
                        w, hh, _, _ = tx[t]
                        R.write_png(os.path.join(out, "tex", f"{t:04x}.png"), w, hh, R.tex_rgba(tx[t]))
            nm = f"{sn}_{i & 0xFFFF:04x}"
            write_obj(os.path.join(out, "obj", nm), nm, tris, texmap)
            ent = [nm, [sn], i & 0xFFFF, len(m["objs"]), sum(len(o["faces"]) for o in m["objs"]),
                   sorted({t[9] for t in tris}), components(m)]
            seen[h] = ent
            if len(tris) <= 1500:
                imgs.append((nm, R.render(tris, size, 0.9, 0.3)))
    with open(os.path.join(out, "index.txt"), "w") as f:
        for nm, sl, mid, no, nf, tl, nc in seen.values():
            f.write(f"{nm}\tstages={','.join(sl[:6])}{'+' if len(sl) > 6 else ''}\tid={mid:04x}"
                    f"\tname={names.get(mid, '')}\tobjs={no}\tfaces={nf}\tcomp={nc}\ttex={','.join(f'{t:04x}' for t in tl)}\n")
    per = cols * cols // 2
    for s in range(0, len(imgs), per):
        W, H, rgb = R.sheet([im for _, im in imgs[s:s + per]], size, cols)
        R.write_png(os.path.join(out, f"sheet_{s // per:02d}.png"), W, H, rgb)
        with open(os.path.join(out, f"sheet_{s // per:02d}.txt"), "w") as f:
            f.write("\n".join(f"{k}: {n}" for k, (n, _) in enumerate(imgs[s:s + per])))
    print(f"modelos únicos {len(seen)}, hojas {(len(imgs) + per - 1) // per}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
