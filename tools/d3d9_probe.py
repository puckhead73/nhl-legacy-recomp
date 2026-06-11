#!/usr/bin/env python3
"""Phase-0 D3D9 hookability probe: walk the generated recomp C++ call graph
(no flat image needed) to characterize the statically-linked D3D9 cluster and
the present chain. Guest->guest calls are emitted as direct sub_XXXXXXXX(ctx,base);
guest functions are defined as DEFINE_REX_FUNC(sub_XXXXXXXX) { with the original
PPC disasm preserved as // comments."""
import re, glob, sys

GEN = 'generated/default/'
files = {}
for f in glob.glob(GEN + 'nhllegacy_recomp.*.cpp'):
    files[f.replace(chr(92), '/')] = open(f, encoding='utf-8', errors='replace').read()

fidx = {}
for f, txt in files.items():
    lst = [(m.start(), int(m.group(1), 16))
           for m in re.finditer(r'DEFINE_REX_FUNC\(sub_([0-9A-Fa-f]{8})\)', txt)]
    lst.sort()
    fidx[f] = lst


def line_to_off(txt, lineno):
    off = 0
    for i, ln in enumerate(txt.split('\n')):
        if i == lineno - 1:
            return off
        off += len(ln) + 1
    return off


def enclosing(path, lineno):
    path = path.replace(chr(92), '/')
    txt = files[path]
    off = line_to_off(txt, lineno)
    cur = '?'
    for pos, a in fidx[path]:
        if pos <= off:
            cur = a
        else:
            break
    return cur


def body(addr):
    for f, lst in fidx.items():
        for i, (pos, a) in enumerate(lst):
            if a == addr:
                end = lst[i + 1][0] if i + 1 < len(lst) else len(files[f])
                return f, files[f][pos:end]
    return None, None


def characterize(addr):
    f, b = body(addr)
    if not b:
        return None
    bl = len(re.findall(r'// bl 0x', b))
    st = len(re.findall(r'REX_STORE', b))
    ld = len(re.findall(r'REX_LOAD', b))
    imp = [i for i in re.findall(r'__imp__(\w+)\(ctx', b) if not i.startswith('sub_')]
    # guest callees
    callees = sorted(set(int(x, 16) for x in re.findall(r'\bsub_([0-9A-Fa-f]{8})\(ctx', b)))
    return dict(file=f.split('/')[-1], bl=bl, stores=st, loads=ld, kcalls=imp, callees=callees)


def callers(addr):
    """All functions that contain a direct call to sub_<addr>."""
    out = []
    pat = re.compile(r'\bsub_%08X\(ctx' % addr)
    for f, txt in files.items():
        for m in pat.finditer(txt):
            # find enclosing function
            cur = None
            for pos, a in fidx[f]:
                if pos <= m.start():
                    cur = a
                else:
                    break
            if cur and cur != addr:
                out.append(cur)
    return sorted(set(out))


if __name__ == '__main__':
    cmd = sys.argv[1] if len(sys.argv) > 1 else 'demo'
    if cmd == 'char':
        for a in [int(x, 16) for x in sys.argv[2:]]:
            c = characterize(a)
            print(f"sub_{a:08X}: {c}" if c else f"sub_{a:08X}: not found")
    elif cmd == 'callers':
        a = int(sys.argv[2], 16)
        cs = callers(a)
        print(f"sub_{a:08X} called by {len(cs)} distinct functions:")
        for c in cs:
            print(f"  sub_{c:08X}")
    elif cmd == 'enc':
        print(f"sub_{enclosing(sys.argv[2], int(sys.argv[3])):08X}")
    else:
        print("present-chain callers of swap/submit sub_827F1C88:")
        for c in callers(0x827F1C88):
            print(f"  sub_{c:08X}")
