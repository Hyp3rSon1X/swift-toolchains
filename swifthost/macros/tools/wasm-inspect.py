#!/usr/bin/env python3
"""Lists sections, imports and exports of a wasm module (no external tools needed)."""
import json, sys
d = open(sys.argv[1], 'rb').read()
p = 8
def leb(p):
    r = s = 0
    while True:
        b = d[p]; p += 1; r |= (b & 0x7f) << s; s += 7
        if not b & 0x80: return r, p
def name(p):
    n, p = leb(p); return d[p:p+n].decode(), p + n
secs, imports, exports = [], [], []
while p < len(d):
    sid = d[p]; size, q = leb(p+1); end = q + size
    nm = None
    if sid == 0: nm, _ = name(q)
    secs.append({"id": sid, "name": nm, "bytes": size})
    if sid == 2:
        n, r = leb(q)
        for _ in range(n):
            m, r = name(r); f, r = name(r); k = d[r]; r += 1
            imports.append(f"{m}.{f}")
            if k == 0: _, r = leb(r)
            elif k == 1: r += 1; fl = d[r]; r += 1; _, r = leb(r); (_, r) = leb(r) if fl & 1 else (0, r)
            elif k == 2: fl = d[r]; r += 1; _, r = leb(r); (_, r) = leb(r) if fl & 1 else (0, r)
            elif k == 3: r += 2
            elif k == 4: r += 1; _, r = leb(r)
    if sid == 7:
        n, r = leb(q)
        for _ in range(n):
            e, r = name(r); k = d[r]; r += 1; _, r = leb(r)
            exports.append({"name": e, "kind": ["func", "table", "memory", "global", "tag"][k]})
    p = end
print(json.dumps({"sections": secs, "imports": imports, "exports": exports}, indent=1))
