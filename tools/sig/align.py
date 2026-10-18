#!/usr/bin/env python3
"""align: completa el match exacto de sigs.py con prefijo único, delta entre anclas y orden.

  align.py <sigs_ref.json> <match.json> <objetivo.exe>   -> names_eu.json (nombre -> dirección, método)
"""
import json,sys,struct,bisect
import os; sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'recomp')); import recomp
sigs=json.load(open(sys.argv[1])); m=json.load(open(sys.argv[2])); exe=recomp.Exe(sys.argv[3])
cs,ce=recomp.detect_code(exe); exe.end,exe.code_start=ce,cs
starts=recomp.find_funcs(exe,[]); S=set(starts)
ref=sorted((int(x['addr'],16),x['name'],x['words']) for x in sigs)
anch={int(v['ref'],16):int(v['target'],16) for v in m.values()}
out={n:int(v['target'],16) for n,v in m.items()}; how={n:'exact' for n in out}
import struct as _st
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__))); import sigs as SG
_, text = SG.load_exe(sys.argv[3]); nw=len(text)//4
allw=[SG.mask(x) for x in _st.unpack_from(f"<{nw}I",text,0)]
pidx={}
for i in range(nw-7): pidx.setdefault(SG.h(allw[i:i+8]),[]).append(i)
used=set(out.values())
for x in sigs:
  n=x['name']
  if n in out: continue
  c=[exe.base+4*i for i in pidx.get(x['prefix'],[])]
  c=[t for t in c if t not in used]
  if len(c)==1:
    out[n]=c[0]; how[n]='prefijo'; used.add(c[0]); anch[int(x['addr'],16)]=c[0]
ak=sorted(anch)
for a,n,w in ref:
  if n in out: continue
  i=bisect.bisect(ak,a)
  if i==0 or i==len(ak): continue
  a1,a2=ak[i-1],ak[i]; d1,d2=anch[a1]-a1,anch[a2]-a2
  if d1==d2 and (a+d1) in S: out[n]=a+d1; how[n]='delta'; continue
  # emparejar por orden si mismas cuentas
  rs=[r for r in ref if a1<r[0]<a2]; es=[s for s in starts if anch[a1]<s<anch[a2]]
  if len(rs)==len(es):
    k=[r[1] for r in rs].index(n); out[n]=es[k]; how[n]='orden'
  else:
    # delta del ancla previa si cae en un start
    if (a+d1) in S: out[n]=a+d1; how[n]='delta_prev'
    elif (a+d2) in S: out[n]=a+d2; how[n]='delta_next'
import collections
print(collections.Counter(how.values()), len(out),'/',len(ref))
tot=sum(r[2] for r in ref); print('cobertura palabras %.1f%%'%(100*sum(r[2] for r in ref if r[1] in out)/tot))
json.dump({n:{'target':f'{t:08x}','how':how[n]} for n,t in out.items()},open('names_eu.json','w'),indent=0)
for k in ['main','GV_Main','StageCharacterEntries']:
  for n in out:
    if n.lower()==k.lower(): print(n,hex(out[n]))
