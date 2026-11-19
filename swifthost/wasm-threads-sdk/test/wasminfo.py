#!/usr/bin/env python3
"""wasminfo.py file.wasm...: memory import/definition/export, shared flag, start function, name-section hits."""
import sys
def leb(b,i):
    r=s=0
    while True:
        x=b[i]; i+=1; r|=(x&0x7f)<<s; s+=7
        if not x&0x80: return r,i
def name(b,i):
    n,i=leb(b,i); return b[i:i+n].decode('utf8','replace'),i+n
def limits(b,i):
    f,i=leb(b,i); mn,i=leb(b,i); mx=None
    if f&1: mx,i=leb(b,i)
    return f,mn,mx,i
for path in sys.argv[1:]:
    b=open(path,'rb').read(); i=8
    imports=[];exports=[];mems=[];start=None;fnames={};nfimp=0
    while i<len(b):
        sid=b[i]; sz,j=leb(b,i+1); end=j+sz
        if sid==2:
            n,j=leb(b,j)
            for _ in range(n):
                m,j=name(b,j); f,j=name(b,j); k=b[j]; j+=1
                if k==0: _,j=leb(b,j); nfimp+=1
                elif k==1: j+=1; _f,_,_,j=limits(b,j)
                elif k==2: fl,mn,mx,j=limits(b,j); imports.append((m,f,'memory',fl,mn,mx))
                elif k==3: j+=2
                else: j+=1; _,j=leb(b,j)
        elif sid==5:
            n,j=leb(b,j)
            for _ in range(n): fl,mn,mx,j=limits(b,j); mems.append((fl,mn,mx))
        elif sid==7:
            n,j=leb(b,j)
            for _ in range(n):
                f,j=name(b,j); k=b[j]; j+=1; idx,j=leb(b,j); exports.append((f,k,idx))
        elif sid==8: start,_=leb(b,j)
        elif sid==0:
            nm,k=name(b,j)
            if nm=='name':
                while k<end:
                    sub=b[k]; ssz,k2=leb(b,k+1); e2=k2+ssz
                    if sub==1:
                        c,k3=leb(b,k2)
                        for _ in range(c): idx,k3=leb(b,k3); s,k3=name(b,k3); fnames[idx]=s
                    k=e2
        i=end
    print(path)
    print("  memory imports :",[(m,f,'shared' if fl&2 else 'unshared',mn,mx) for m,f,_,fl,mn,mx in imports])
    print("  memory defined :",[('shared' if fl&2 else 'unshared',mn,mx) for fl,mn,mx in mems])
    print("  memory exports :",[(f) for f,k,_ in exports if k==2])
    print("  start function :",start,fnames.get(start))
    print("  name section has __wasm_init_memory:",'__wasm_init_memory' in fnames.values(),"| functions named:",len(fnames))
