"""Backward dataflow slice on a spirv-dis listing: does the fragment output value
depend on a given texture's sample result(s)? Pure static reachability over SSA ids.
Usage: python tools/_spirv_slice.py <file.ps.dis> <texture_name_substr>
"""
import re, sys, collections
dis = open(sys.argv[1]).read().splitlines()
texsub = sys.argv[2]

idre = re.compile(r'%[\w]+')
# Parse: result_id (if any) and operand ids per instruction line.
# Forms:  "%res = OpFoo %a %b ..."  or  "OpStore %ptr %val"
defs = {}        # result_id -> (opcode, [operand ids])
stores = []      # (ptr, val)
sample_ids = {}  # result_id -> the texture/sampler operand names involved (for tex tagging)
load_of = {}     # result_id -> loaded-from id (OpLoad)
for ln in dis:
    s = ln.strip()
    if not s or s.startswith(';'): continue
    m = re.match(r'(%[\w]+)\s*=\s*Op(\w+)\s*(.*)', s)
    if m:
        res, op, rest = m.group(1), m.group(2), m.group(3)
        ids = idre.findall(rest)
        defs[res] = (op, ids)
        if op == 'Load' and ids:
            load_of[res] = ids[0]
        if op.startswith('Image') and 'Sample' in op or op.startswith('ImageFetch'):
            sample_ids[res] = ids
    else:
        m2 = re.match(r'Op(\w+)\s+(.*)', s)
        if m2 and m2.group(1) == 'Store':
            ids = idre.findall(m2.group(2))
            if len(ids) >= 2:
                stores.append((ids[0], ids[1]))

# Find the output store: OpStore %xe_out_fragment_data_0 %val
out_stores = [v for (p, v) in stores if 'out_fragment_data_0' in p]
print(f"output stores to xe_out_fragment_data_0: {len(out_stores)} -> values {out_stores}")

# Identify sample results that used the target texture. A sampled-image is built via
# OpSampledImage %ty %img %smp; the sample op references that. Walk operands to names.
def names_in(res, depth=0, seen=None):
    if seen is None: seen=set()
    if res in seen or depth>6: return set()
    seen.add(res)
    out=set([res])
    op, ids = defs.get(res, (None, []))
    for i in ids:
        out |= names_in(i, depth+1, seen)
    return out

target_samples = []
for res, ids in sample_ids.items():
    nm = names_in(res)
    if any(texsub in n for n in nm):
        target_samples.append(res)
print(f"sample ops whose inputs touch '{texsub}': {target_samples}")

# Backward slice from each output store value; collect all reachable ids.
def slice_back(root):
    seen=set(); stack=[root]
    while stack:
        x=stack.pop()
        if x in seen: continue
        seen.add(x)
        op, ids = defs.get(x, (None, []))
        for i in ids: stack.append(i)
    return seen

allslice=set()
for v in out_stores:
    allslice |= slice_back(v)
print(f"backward slice from output: {len(allslice)} ids")
hit = [t for t in target_samples if t in allslice]
print(f"\nRESULT: of {len(target_samples)} '{texsub}' sample(s), {len(hit)} reach the fragment output: {hit}")
