#!/usr/bin/env python3
"""Resolve FEX sampling-profiler output (see docs/how-it-works.md).
usage: resolve_samples.py TAG FEX_UNSTRIPPED [NM]   expects samplesTAG.bin, perfTAG.map, mapsTAG.txt in the current directory."""
import struct, bisect, collections, subprocess, sys
tag=sys.argv[1]; FEXBIN=sys.argv[2]; NM=sys.argv[3] if len(sys.argv)>3 else 'llvm-nm'
ent=[]
for l in open('perf%s.map'%tag):
    a,s,n=l.rstrip('\n').split(' ',2); ent.append((int(a,16),int(s,16),n))
ent.sort(); starts=[e[0] for e in ent]
def lib(pc):
    i=bisect.bisect_right(starts,pc)-1
    if i>=0 and ent[i][0]<=pc<ent[i][0]+ent[i][1]: return ent[i][2].split('/')[-1]
base=None
for l in open('maps%s.txt'%tag):
    if l.rstrip().endswith('/FEX'):
        a=int(l.split('-')[0],16); base=a if base is None else min(base,a)
nm=subprocess.run([NM,'-n','--defined-only','-C',FEXBIN],capture_output=True,text=True).stdout.splitlines()
syms=[]
for l in nm:
    p=l.split(None,2)
    if len(p)==3:
        try: syms.append((int(p[0],16),p[2]))
        except: pass
sa=[s[0] for s in syms]
def fexsym(pc):
    if base is None: return None
    off=pc-base
    if 0<=off<0x3000000:
        i=bisect.bisect_right(sa,off)-1
        return syms[i][1][:70] if i>=0 else None
maps=[]
for l in open('maps%s.txt'%tag):
    p=l.split(); a,b=p[0].split('-'); maps.append((int(a,16),int(b,16),p[5] if len(p)>5 else '[anon]'))
def mapname(pc):
    for a,b,n in maps:
        if a<=pc<b: return n.split('/')[-1]
d=open('samples%s.bin'%tag,'rb').read()
byt=collections.defaultdict(collections.Counter); tot=collections.Counter()
for i in range(0,len(d),16):
    pc,info=struct.unpack_from('<QQ',d,i)
    if pc==0: break
    tid=info&0xffffffff
    k=lib(pc)
    if k: key='JIT '+k
    else:
        s=fexsym(pc); key=('FEX '+s) if s else 'host '+str(mapname(pc))
    byt[tid][key]+=1; tot[tid]+=1
for tid,n in tot.most_common(6):
    print("== tid",tid,n,"samples")
    for k,c in byt[tid].most_common(10): print("  %5.1f%% %s"%(100*c/n,k))
