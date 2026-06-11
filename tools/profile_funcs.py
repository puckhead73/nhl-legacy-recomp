#!/usr/bin/env python3
"""Profile a list of recompiled functions for fixed-point arithmetic density,
to find the VP6 inverse-transform / dequant kernel among codec vtable methods."""
import os, re, sys, glob

GEN = "E:/Repositories/nhl-legacy-recomp/generated/default"

# Build an index: function name -> (file, start_line, end_line)
def body(fnname):
    for f in glob.glob(os.path.join(GEN, "*.cpp")):
        data = open(f, encoding='utf-8', errors='ignore').read().splitlines()
        for i, l in enumerate(data):
            if f"DEFINE_REX_FUNC({fnname})" in l:
                j = i
                depth = 0
                while j < len(data):
                    if data[j].strip() == '}' :
                        return data[i:j+1], os.path.basename(f)
                    j += 1
        # not found in this file
    return None, None

PATS = {
    'mullw': r'\bmullw\b', 'mulhw': r'\bmulhw', 'shift': r'\b(srawi|sraw|slw|srw|rlwinm|rlwimi)\b',
    'addsub': r'\b(add|subf|addi|subfic)\b', 'load': r'REX_LOAD_U', 'store': r'REX_STORE_U',
    'vmx': r'\b(vadd|vsub|vmul|vpksh|vmrg|lvx|stvx|vperm|vsel)\w*', 'indcall': r'REX_CALL_INDIRECT',
    'call': r'\bsub_[0-9A-Fa-f]{8}\(ctx',
}

for fn in sys.argv[1:]:
    src, fname = body(fn)
    if not src:
        print(f"{fn}: NOT FOUND")
        continue
    text = '\n'.join(src)
    # count over the comment lines (the disasm mnemonics are in // comments)
    counts = {k: len(re.findall(p, text)) for k, p in PATS.items()}
    nlines = len(src)
    print(f"{fn} [{fname} {nlines}L]  " +
          "  ".join(f"{k}={counts[k]}" for k in PATS))
