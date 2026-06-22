#!/usr/bin/env python3
"""EA-toolchain jump-table detector for NHL 12 (nhlzf.exe).

XenonAnalyse's stock masks require exactly-consecutive instruction sequences;
EA's compiler interleaves nops and reorders address materialization, so stock
detection finds 0 tables. This tool does a structural backward walk from every
`bctr` in .text, recognizing the dispatch *roles* regardless of scheduling:

    cmplwi crN, rIDX, COUNT          ; bounds check (up to 32 insns back)
    bgt    crN, default / ble ...
    ...
    lis    rT, hi ; addi rT, rT, lo  ; table address  (nops interleaved)
    [slwi  rX, rIDX, 1]              ; halfword tables scale the index
    lbzx/lhzx rO, rT, rIDX|rX        ; load offset (entry size 1 or 2)
    [slwi  rO, rO, SH]               ; optional scale of loaded offset
    lis    rB, hi ; addi rB, rB, lo  ; label base    (nops interleaved)
    add    rS, rB, rO | add rS, rO, rB
    mtctr  rS
    bctr

Labels = labelBase + (entry << SH). Emits XenonRecomp-compatible TOML with
base = address of the bctr (recompiler arms the entry at that PC).

Usage: python ea_jumptables.py extracted/nhlzf_image.bin config/switch_tables.toml
"""

import struct
import sys

IMAGE_BASE = 0x82000000
TEXT_VA = 0x82580000
TEXT_SIZE = 0x01158BD0
TEXT_END = TEXT_VA + TEXT_SIZE
NOP = 0x60000000
BCTR = 0x4E800420

MAX_WALK = 48          # instructions to walk back from bctr for the dispatch
MAX_BOUNDS_WALK = 64   # additional walk for cmplwi/bgt bounds check
MAX_LABELS = 4096      # sanity cap on table size


def op(w):       return (w >> 26) & 0x3F
def rd(w):       return (w >> 21) & 0x1F
def ra(w):       return (w >> 16) & 0x1F
def rb(w):       return (w >> 11) & 0x1F
def xo(w):       return (w >> 1) & 0x3FF
def simm(w):
    v = w & 0xFFFF
    return v - 0x10000 if v & 0x8000 else v
def uimm(w):     return w & 0xFFFF


def is_nop(w):   return w == NOP
def is_lis(w):   return op(w) == 15 and ra(w) == 0           # addis rD,0,imm
def is_addis(w): return op(w) == 15
def is_addi(w):  return op(w) == 14
def is_ori_self(w):  # ori rA, rS, imm with rA==rS (low-half materialization)
    return op(w) == 24 and ra(w) == rd(w)
def is_lbzx(w):  return op(w) == 31 and xo(w) == 87
def is_lhzx(w):  return op(w) == 31 and xo(w) == 279
def is_lwzx(w):  return op(w) == 31 and xo(w) == 23
def is_add(w):   return op(w) == 31 and xo(w) == 266
def is_mtctr(w):
    # mtspr CTR: xo=467, spr field (swapped halves) == 9
    if op(w) != 31 or xo(w) != 467:
        return False
    spr = ((w >> 16) & 0x1F) | (((w >> 11) & 0x1F) << 5)
    return spr == 9
def is_slwi(w):
    # rlwinm rA,rS,SH,0,31-SH
    if op(w) != 21:
        return False
    sh = (w >> 11) & 0x1F
    mb = (w >> 6) & 0x1F
    me = (w >> 1) & 0x1F
    return mb == 0 and me == 31 - sh
def slwi_sh(w):  return (w >> 11) & 0x1F
def is_cmplwi(w):
    return op(w) == 10 and ((w >> 21) & 1) == 0  # L=0
def cmplwi_cr(w):  return (w >> 23) & 7
def is_bc(w):    return op(w) == 16
def is_bclr(w):  return op(w) == 19 and xo(w) == 16


class Insn:
    __slots__ = ("va", "w")
    def __init__(self, va, w):
        self.va = va
        self.w = w


def read_u32(data, va):
    return struct.unpack_from(">I", data, va - IMAGE_BASE)[0]


def walk_back(data, va, count):
    """Return up to `count` instructions before va, most recent first."""
    out = []
    for i in range(1, count + 1):
        v = va - 4 * i
        if v < TEXT_VA:
            break
        out.append(Insn(v, read_u32(data, v)))
    return out


def materialize(insns, start_idx, reg, allow_addis_pair=True):
    """Walk back from insns[start_idx] resolving `reg` = lis+addi/ori pair.
    Returns (value, idx_of_lis) or None. Tolerates interleaved insns that
    don't write `reg`."""
    lo = None
    lo_kind = None
    for i in range(start_idx, len(insns)):
        w = insns[i].w
        if is_nop(w):
            continue
        if lo is None:
            if is_addi(w) and rd(w) == reg and ra(w) == reg:
                lo, lo_kind = simm(w), "addi"
            elif is_ori_self(w) and ra(w) == reg:
                lo, lo_kind = uimm(w), "ori"
            elif is_lis(w) and rd(w) == reg:
                # lis with no low half (rare)
                return ((simm(w) << 16) & 0xFFFFFFFF, i)
            elif writes_gpr(w) == reg:
                return None  # clobbered by something else
        else:
            if is_lis(w) and rd(w) == reg:
                hi = simm(w) << 16
                return ((hi + lo) & 0xFFFFFFFF, i)
            if writes_gpr(w) == reg:
                return None
    return None


def writes_gpr(w):
    """Best-effort: which GPR does this insn write? (None if unknown/none)"""
    o = op(w)
    if o in (14, 15, 24, 25, 26, 27, 28, 29):  # addi/addis/ori../andi..
        return ra(w) if o >= 24 else rd(w)
    if o == 21:  # rlwinm writes rA
        return ra(w)
    if o in (32, 34, 40, 42):  # lwz/lbz/lhz/lha
        return rd(w)
    if o == 31:
        x = xo(w)
        if x in (23, 87, 279, 266, 40, 8, 10, 235, 444, 28, 60, 124, 284, 316):
            # loads/add/subf/mull/or/and/... write rD or rA depending; treat
            # rD-writers; 'or' (444) writes rA
            if x == 444:
                return ra(w)
            return rd(w)
    return None


def bounds_via_xref(data, bctr_va, r_index):
    """Fallback for dispatch blocks entered by branch: the cmplwi/bgt lives at
    the branch source, not linearly above the bctr. Scan .text for a
    conditional branch into the dispatch window and read the bounds check
    just before it. (Rare — one known site in nhlzf.)"""
    lo = bctr_va - MAX_WALK * 4
    for off in range(TEXT_VA - IMAGE_BASE, TEXT_END - IMAGE_BASE, 4):
        w = struct.unpack_from(">I", data, off)[0]
        if op(w) != 16 or (w & 2):
            continue
        bd = w & 0xFFFC
        if bd & 0x8000:
            bd -= 0x10000
        src = IMAGE_BASE + off
        tgt = src + bd
        if not (lo <= tgt < bctr_va) or abs(src - bctr_va) <= MAX_WALK * 4:
            continue
        # look for cmplwi on r_index within 8 insns before the branch source
        count = None
        default = 0
        for ins in walk_back(data, src, 8):
            iw = ins.w
            if is_cmplwi(iw) and ra(iw) == r_index:
                count = uimm(iw) + 1
            if is_bc(iw):
                bo = (iw >> 21) & 0x1F
                bi = (iw >> 16) & 0x1F
                if bi % 4 == 1 and (bo & ~1) in (4, 12) and (bo & ~1) == 12:
                    d = iw & 0xFFFC
                    if d & 0x8000:
                        d -= 0x10000
                    default = ins.va + d
        if count is not None:
            return count, default
    return None, 0


def analyze_site(data, bctr_va):
    insns = walk_back(data, bctr_va, MAX_WALK)
    if not insns:
        return None

    # 1) mtctr rS immediately before bctr (allow nops)
    i = 0
    while i < len(insns) and is_nop(insns[i].w):
        i += 1
    if i >= len(insns) or not is_mtctr(insns[i].w):
        return None
    rS = rd(insns[i].w)
    i += 1

    # 2) add rS, x, y
    while i < len(insns) and (is_nop(insns[i].w) or writes_gpr(insns[i].w) not in (rS,)):
        if not is_nop(insns[i].w) and writes_gpr(insns[i].w) == rS:
            break
        if not is_nop(insns[i].w) and is_add(insns[i].w) and rd(insns[i].w) == rS:
            break
        i += 1
    if i >= len(insns) or not is_add(insns[i].w) or rd(insns[i].w) != rS:
        return None
    add_idx = i
    a, b = ra(insns[i].w), rb(insns[i].w)
    i += 1

    # 3) find the offset load (lbzx/lhzx) among following (earlier) insns;
    #    determine which of a/b is the offset reg and optional post-load slwi.
    shift = 0
    load_idx = None
    rO = None
    entry_size = None
    rT = None
    rIdxRaw = None
    for j in range(i, len(insns)):
        w = insns[j].w
        if is_nop(w):
            continue
        if is_slwi(w) and ra(w) in (a, b) and rd(w) == ra(w) is None:
            pass
        if is_slwi(w) and ra(w) in (a, b):
            # slwi rO, rO, sh scaling the loaded offset
            shift = slwi_sh(w)
            continue
        if (is_lbzx(w) or is_lhzx(w)) and rd(w) in (a, b):
            load_idx = j
            rO = rd(w)
            entry_size = 1 if is_lbzx(w) else 2
            rT = ra(w)
            rIdxRaw = rb(w)
            break
        # unrelated instruction is fine unless it clobbers a/b
        wr = writes_gpr(w)
        if wr in (a, b):
            # the base-materialization addi/lis also "clobbers" — that's the
            # base reg; keep going to find the load of the *other* reg
            continue
    if load_idx is None:
        return None
    rBase = b if rO == a else a

    # 4) label base = lis/addi into rBase, searched from just after add
    #    backward through the window (materialization happens before mtctr,
    #    between or before the load — search the whole window)
    base_val = materialize(insns, i, rBase)
    if base_val is None:
        # base may be materialized before the add but after load in walk
        # order; try from add_idx+1
        base_val = materialize(insns, add_idx + 1, rBase)
    if base_val is None:
        return None
    label_base, _ = base_val

    # 5) table address = lis/addi into rT, searched from after the load
    tbl_val = materialize(insns, load_idx + 1, rT)
    if tbl_val is None:
        return None
    table_va, _ = tbl_val

    # 6) index register: for halfword tables EA pre-scales with slwi rX,rIDX,1
    r_index = rIdxRaw
    if entry_size == 2:
        for j in range(load_idx + 1, len(insns)):
            w = insns[j].w
            if is_nop(w):
                continue
            if is_slwi(w) and ra(w) == rIdxRaw and slwi_sh(w) == 1:
                r_index = rd(w) if False else (w >> 21 & 0x1F)  # rS field
                r_index = (w >> 21) & 0x1F
                break
            if writes_gpr(w) == rIdxRaw:
                break

    # 7) bounds check: cmplwi crN, r_index, COUNT (+ bgt/ble for default)
    bounds = walk_back(data, bctr_va, MAX_BOUNDS_WALK)
    count = None
    default = 0
    cr_want = None
    for ins in bounds:
        if ins.va >= bctr_va:
            continue
        w = ins.w
        if cr_want is None and (is_bc(w) or is_bclr(w)):
            bo = (w >> 21) & 0x1F
            bi = (w >> 16) & 0x1F
            # branch on GT bit: bi % 4 == 1; taken (bgt: bo=12) or not (ble: bo=4)
            if bi % 4 == 1 and (bo & ~1) in (4, 12):
                cr_want = bi // 4
                if is_bc(w) and (bo & ~1) == 12:
                    tgt = w & 0xFFFC
                    tgt = tgt - 0x10000 if tgt & 0x8000 else tgt
                    default = (ins.va + tgt) if not (w & 2) else tgt
                continue
        if cr_want is not None and is_cmplwi(w) and cmplwi_cr(w) == cr_want:
            if ra(w) == r_index:
                count = uimm(w) + 1
                break
            else:
                # compared a different reg — could be a copy; accept with note
                count = uimm(w) + 1
                break
    if count is None:
        count, default = bounds_via_xref(data, bctr_va, r_index)
    if count is None or count > MAX_LABELS:
        return None

    # 8) read the table & compute labels
    if not (IMAGE_BASE <= table_va < TEXT_END):
        return None
    labels = []
    for k in range(count):
        if entry_size == 1:
            e = data[table_va - IMAGE_BASE + k]
        else:
            e = struct.unpack_from(">H", data, table_va - IMAGE_BASE + 2 * k)[0]
        lab = (label_base + (e << shift)) & 0xFFFFFFFF
        if not (TEXT_VA <= lab < TEXT_END) or lab & 3:
            return None  # implausible — reject whole table
        labels.append(lab)

    return {
        "base": bctr_va,
        "r": r_index,
        "default": default,
        "labels": labels,
        "table_va": table_va,
        "entry_size": entry_size,
        "shift": shift,
    }


def main():
    image_path, out_path = sys.argv[1], sys.argv[2]
    data = open(image_path, "rb").read()

    sites = []
    for off in range(TEXT_VA - IMAGE_BASE, TEXT_END - IMAGE_BASE, 4):
        if struct.unpack_from(">I", data, off)[0] == BCTR:
            sites.append(IMAGE_BASE + off)

    found, rejected = [], []
    for va in sites:
        try:
            r = analyze_site(data, va)
        except (struct.error, IndexError):
            r = None
        if r:
            found.append(r)
        else:
            rejected.append(va)

    with open(out_path, "w") as f:
        f.write("# Generated by ea_jumptables.py (EA/nhlzf structural detector)\n")
        f.write(f"# {len(found)} switch tables from {len(sites)} bctr sites\n\n")
        for t in found:
            f.write("[[switch]]\n")
            f.write(f"base = 0x{t['base']:X}\n")
            f.write(f"r = {t['r']}\n")
            f.write(f"default = 0x{t['default']:X}\n")
            f.write("labels = [\n")
            for lab in t["labels"]:
                f.write(f"    0x{lab:X},\n")
            f.write("]\n\n")

    n_b = sum(1 for t in found if t["entry_size"] == 1)
    n_h = sum(1 for t in found if t["entry_size"] == 2)
    total_labels = sum(len(t["labels"]) for t in found)
    print(f"bctr sites:      {len(sites)}")
    print(f"tables detected: {len(found)}  (byte:{n_b} half:{n_h}, "
          f"{total_labels} labels)")
    print(f"unmatched bctr:  {len(rejected)} (vtable dispatch/tail calls "
          f"expected; review subset)")
    print(f"wrote {out_path}")


if __name__ == "__main__":
    main()
