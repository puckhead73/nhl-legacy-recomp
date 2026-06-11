#!/usr/bin/env python3
"""Phase-0/M1 D3D9 entry-point labeler. Over the generated recomp call graph,
for each of the 181 out-of-line graphics-lib entry points compute the set of
kernel imports it reaches (transitively, following library-internal callees),
plus structural signals (atomic refcount, PM4 draw-packet writes). Bucket by
signature to label Present / CreateDevice / resource creators / Draw / state.

No flat image needed — reads generated/default/nhllegacy_recomp.*.cpp."""
import re, glob, collections

GEN = 'generated/default/'
BS = chr(92)
files = {}
for f in glob.glob(GEN + 'nhllegacy_recomp.*.cpp'):
    files[f.replace(BS, '/')] = open(f, encoding='utf-8', errors='replace').read()

# function definition index, callee/import extraction
defined = {}            # addr -> file
fbody = {}              # addr -> body text
for f, txt in files.items():
    starts = [(m.start(), int(m.group(1), 16))
              for m in re.finditer(r'DEFINE_REX_FUNC\(sub_([0-9A-Fa-f]{8})\)', txt)]
    starts.sort()
    for i, (pos, a) in enumerate(starts):
        end = starts[i + 1][0] if i + 1 < len(starts) else len(txt)
        defined[a] = f
        fbody[a] = txt[pos:end]

LIB = defined[0x827F1C88]
libfns = sorted(a for a, f in defined.items() if f == LIB)


def callees(addr):
    b = fbody.get(addr, '')
    return set(int(x, 16) for x in re.findall(r'\bsub_([0-9A-Fa-f]{8})\(ctx', b))


def imports(addr):
    b = fbody.get(addr, '')
    return set(i for i in re.findall(r'__imp__(\w+)\(ctx', b) if not i.startswith('sub_'))


# transitive kernel imports, following callees that stay inside the lib TU
trans_cache = {}
def trans_imports(addr, seen=None):
    if addr in trans_cache:
        return trans_cache[addr]
    if seen is None:
        seen = set()
    if addr in seen:
        return set()
    seen.add(addr)
    out = set(imports(addr))
    for c in callees(addr):
        if defined.get(c) == LIB:
            out |= trans_imports(c, seen)
    trans_cache[addr] = out
    return out


# external (game-code) callers per lib fn
ext_callers = collections.defaultdict(set)
for f, txt in files.items():
    starts = [(m.start(), int(m.group(1), 16))
              for m in re.finditer(r'DEFINE_REX_FUNC\(sub_([0-9A-Fa-f]{8})\)', txt)]
    starts.sort()
    for m in re.finditer(r'\bsub_([0-9A-Fa-f]{8})\(ctx', txt):
        c = int(m.group(1), 16)
        if defined.get(c) != LIB:
            continue
        enc = None
        for pos, a in starts:
            if pos <= m.start():
                enc = a
            else:
                break
        if enc is not None and f != LIB:
            ext_callers[c].add(enc)

entries = sorted(ext_callers.keys())


def structural(addr):
    b = fbody.get(addr, '')
    sig = []
    if re.search(r'lwarx|stwcx', b):
        sig.append('ATOMIC')
    # PM4 type-3 draw packet header: DRAW_INDX 0x22 / DRAW_INDX_2 0x36 in (op<<8)
    if re.search(r'0x2200\b|0x2201\b|0x3600\b|0x3601\b', b):
        sig.append('PM4DRAW?')
    return sig


# signature import sets that suggest D3D9 roles
ROLE_HINTS = [
    ('PRESENT/SWAP', {'VdSwap', 'VdPersistDisplay'}),
    ('DEVICE-INIT', {'VdInitializeRingBuffer', 'VdInitializeEngines', 'VdSetGraphicsInterruptCallback'}),
    ('CMDBUF', {'VdGetSystemCommandBuffer', 'VdEnableRingBufferRPtrWriteBack'}),
    ('ALLOC/RESOURCE', {'MmAllocatePhysicalMemory', 'MmAllocatePhysicalMemoryEx', 'MmGetPhysicalAddress'}),
    ('FREE/RESOURCE', {'MmFreePhysicalMemory'}),
    ('QUERY/SYNC', {'KeWaitForSingleObject', 'KeSetEvent', 'VdQueryVideoFlags'}),
]


def role(addr):
    ti = trans_imports(addr)
    roles = [name for name, s in ROLE_HINTS if ti & s]
    return roles, ti


def direct_imports(addr):
    return sorted(imports(addr))


def body_metrics(addr):
    b = fbody.get(addr, '')
    stores = len(re.findall(r'REX_STORE', b))
    loads = len(re.findall(r'REX_LOAD', b))
    bl = len(re.findall(r'// bl 0x', b))
    # crude instruction count = number of // comment lines
    insns = len(re.findall(r'\n\s*//', b))
    return insns, stores, loads, bl


# direct-import anchors (more discriminating than transitive)
DSWAP = {'VdSwap', 'VdPersistDisplay'}
DDEVINIT = {'VdInitializeRingBuffer', 'VdInitializeEngines', 'VdSetGraphicsInterruptCallback',
            'VdGetSystemCommandBuffer', 'VdEnableRingBufferRPtrWriteBack', 'VdQueryVideoMode',
            'VdSetDisplayMode', 'VdInitializeScalerCommandBuffer'}
DALLOC = {'MmAllocatePhysicalMemory', 'MmAllocatePhysicalMemoryEx', 'MmFreePhysicalMemory',
          'MmGetPhysicalAddress'}

if __name__ == '__main__':
    print(f"LIB TU: {LIB.split('/')[-1]}  entry points: {len(entries)}\n")
    labeled = []
    for a in entries:
        di = set(direct_imports(a))
        n = len(ext_callers[a])
        ins, st, ld, bl = body_metrics(a)
        atomic = bool(re.search(r'lwarx|stwcx', fbody.get(a, '')))
        if di & DSWAP:
            lab = 'PRESENT/SWAP'
        elif di & DDEVINIT:
            lab = 'DEVICE-INIT'
        elif di & DALLOC:
            lab = 'ALLOC/RESOURCE'
        elif atomic:
            lab = 'COM-REFCOUNT(AddRef/Release/QI)'
        elif not di:
            lab = 'DATAPLANE(no-import: Set*/Draw/state)'
        else:
            lab = 'MISC(' + ','.join(sorted(di)[:3]) + ')'
        labeled.append((a, n, lab, ins, st, ld, bl, sorted(di)[:5]))

    bucket = collections.Counter(l[2].split('(')[0] for l in labeled)
    print("== entry-point buckets (direct-import + structure) ==")
    for k, c in bucket.most_common():
        print(f"  {k}: {c}")

    print("\n== PRESENT / DEVICE-INIT / ALLOC / COM-REFCOUNT (import-anchored, high confidence) ==")
    for a, n, lab, ins, st, ld, bl, di in sorted(labeled, key=lambda r: -r[1]):
        if lab.startswith(('PRESENT', 'DEVICE', 'ALLOC', 'COM')):
            print(f"  sub_{a:08X} callers={n:<3} insns~{ins:<4} st={st:<3} bl={bl:<2} {lab} imps={di}")

    print("\n== DATA-PLANE shortlist (no kernel imports; Set*/Draw/state) — top 30 by callers ==")
    dp = [l for l in labeled if l[2].startswith('DATAPLANE')]
    for a, n, lab, ins, st, ld, bl, di in sorted(dp, key=lambda r: -r[1])[:30]:
        print(f"  sub_{a:08X} callers={n:<3} insns~{ins:<4} stores={st:<3} loads={ld:<3} bl={bl}")
    print(f"  (DATA-PLANE total: {len(dp)})")
