"""C-5g PIXEL-SHADER INSTRUMENTER. Rewrite a captured draw's translated SPIR-V pixel
shader so its FRAGMENT OUTPUT is forced to an intermediate value — letting a replay
DUMP that intermediate as an image (paired with NHL_HIGHCUT_DEBUG_PS + _C5_SHOT). The
goal: see the jersey number-composition layer directly and find where it is overwritten.

The Xenos->SPIR-V translator keeps shader results in a Function-local register file
`xe_var_registers` (array of 12 vec4). The number is composited into ONE of those 12
registers, then the lighting epilogue reads it. By forcing the output = registers[K]
for each K, we can SEE which register holds the number and whether a later stage
discards it. (Pure static slicing can't: the register file is memory, not SSA.)

How it works: disassemble (spirv-dis), insert a final override right before the single
OpReturn —
    %p = OpAccessChain %_ptr_Function_v4float %xe_var_registers %int_K
    %v = OpLoad %v4float %p
         OpStore %xe_out_fragment_data_0 <v, alpha forced 1>
— then reassemble (spirv-as). The override is the LAST write to the output, so it wins
regardless of the epilogue. The draw's descriptor interface is untouched, so all
texture/sampler/constant bindings still line up at replay.

Usage:
  # output = registers[8].rgb (alpha forced opaque)
  python tools/_instrument_ps.py in.ps.spv out.spv --reg 8
  # output = registers[8] alpha as grayscale (see the glyph COVERAGE)
  python tools/_instrument_ps.py in.ps.spv out.spv --reg 8 --alpha
  # step every register -> out_reg0.spv .. out_reg11.spv (pass out WITHOUT .spv)
  python tools/_instrument_ps.py in.ps.spv out --step
  # force output = an arbitrary SSA id (best-effort; must dominate the return)
  python tools/_instrument_ps.py in.ps.spv out.spv --ssa %1265
"""
import argparse
import os
import subprocess
import sys

DIS = r"C:/VulkanSDK/1.4.350.0/Bin/spirv-dis.exe"
ASM = r"C:/VulkanSDK/1.4.350.0/Bin/spirv-as.exe"
NREG = 12


def disasm(spv):
    return subprocess.check_output([DIS, "--no-color", spv], text=True).splitlines()


def asm(lines, out):
    txt = "\n".join(lines) + "\n"
    p = subprocess.run([ASM, "--target-env", "vulkan1.3", "-", "-o", out],
                       input=txt, text=True, capture_output=True)
    if p.returncode != 0:
        # retry without target-env pin (older/newer toolchains)
        p2 = subprocess.run([ASM, "-", "-o", out], input=txt, text=True, capture_output=True)
        if p2.returncode != 0:
            sys.stderr.write(p.stderr + p2.stderr)
            raise SystemExit(f"spirv-as failed for {out}")


def build_override(reg=None, ssa=None, alpha=False, tag="", scalar=None):
    """SPIR-V text lines that force the fragment output, inserted before OpReturn."""
    L = []
    if scalar is not None:
        # broadcast a float SSA to a visible grayscale (value 0 -> black, 1 -> white)
        out = f"%dbgo{tag}"
        L.append(f"        {out} = OpCompositeConstruct %v4float {scalar} {scalar} {scalar} %float_1")
        L.append(f"               OpStore %xe_out_fragment_data_0 {out}")
        return L
    if ssa is not None:
        valid = ssa
    else:
        p = f"%dbgp{tag}"
        valid = f"%dbgv{tag}"
        L.append(f"        {p} = OpAccessChain %_ptr_Function_v4float %xe_var_registers %int_{reg}")
        L.append(f"        {valid} = OpLoad %v4float {p}")
    out = f"%dbgo{tag}"
    if alpha:
        a = f"%dbga{tag}"
        L.append(f"        {a} = OpCompositeExtract %float {valid} 3")
        L.append(f"        {out} = OpCompositeConstruct %v4float {a} {a} {a} %float_1")
    else:
        L.append(f"        {out} = OpCompositeInsert %v4float %float_1 {valid} 3")
    L.append(f"               OpStore %xe_out_fragment_data_0 {out}")
    return L


def instrument(lines, reg=None, ssa=None, alpha=False, scalar=None):
    # Insert the override before the single OpReturn in %main.
    out = []
    inserted = False
    for ln in lines:
        if not inserted and ln.strip() == "OpReturn":
            out.extend(build_override(reg=reg, ssa=ssa, alpha=alpha, tag="X", scalar=scalar))
            inserted = True
        out.append(ln)
    if not inserted:
        raise SystemExit("no OpReturn found — unexpected shader shape")
    return out


def instrument_capture(lines, reg, after_ssa, alpha=False):
    """CAPTURE-AT-POINT: snapshot registers[reg] right AFTER the instruction that
    defines %after_ssa, stash it in a debug Function var, and output it at the end.
    This reveals the value of a register at a SPECIFIC program point (not just at
    end-of-shader) — needed because the translator's register file is reused, so the
    end-of-shader value of a register is not what it held mid-shader."""
    import re
    out = []
    declared = False
    captured = False
    returned = False
    anchor = re.compile(r'^\s*' + re.escape(after_ssa) + r'\s*=')
    for ln in lines:
        out.append(ln)
        # declare the debug var right after the register file is declared (same first block)
        if not declared and 'xe_var_registers = OpVariable' in ln:
            out.append("%dbgcapvar = OpVariable %_ptr_Function_v4float Function")
            declared = True
        # snapshot registers[reg] right after the anchor instruction
        if not captured and declared and anchor.match(ln):
            out.append(f"        %dbgcp = OpAccessChain %_ptr_Function_v4float %xe_var_registers %int_{reg}")
            out.append(f"        %dbgcv = OpLoad %v4float %dbgcp")
            out.append(f"               OpStore %dbgcapvar %dbgcv")
            captured = True
    if not captured:
        raise SystemExit(f"anchor {after_ssa} not found")
    # second pass: emit the output override before OpReturn
    final = []
    for ln in out:
        if not returned and ln.strip() == "OpReturn":
            final.append("        %dbgfin = OpLoad %v4float %dbgcapvar")
            if alpha:
                final.append("        %dbgfa = OpCompositeExtract %float %dbgfin 3")
                final.append("        %dbgfo = OpCompositeConstruct %v4float %dbgfa %dbgfa %dbgfa %float_1")
            else:
                final.append("        %dbgfo = OpCompositeInsert %v4float %float_1 %dbgfin 3")
            final.append("               OpStore %xe_out_fragment_data_0 %dbgfo")
            returned = True
        final.append(ln)
    return final


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("inspv")
    ap.add_argument("out", help="output .spv path (or prefix when --step)")
    ap.add_argument("--reg", type=int, default=None, help="output = registers[K]")
    ap.add_argument("--ssa", default=None, help="output = this SSA id (e.g. %%1265)")
    ap.add_argument("--alpha", action="store_true", help="show the alpha channel as grayscale")
    ap.add_argument("--step", action="store_true", help="emit one .spv per register 0..11")
    ap.add_argument("--cap", type=int, default=None, help="capture-at-point: register to snapshot")
    ap.add_argument("--after", default=None, help="capture-at-point: anchor SSA id (e.g. %%1417)")
    ap.add_argument("--scalar", default=None, help="output = a float SSA broadcast to grayscale (e.g. %%1377)")
    args = ap.parse_args()

    base = disasm(args.inspv)
    if args.cap is not None:
        if not args.after:
            raise SystemExit("--cap requires --after %SSA")
        mod = instrument_capture(base, reg=args.cap, after_ssa=args.after, alpha=args.alpha)
        asm(mod, args.out)
        print(f"capture registers[{args.cap}] after {args.after} -> {args.out}")
        return 0
    if args.step:
        prefix = args.out[:-4] if args.out.endswith(".spv") else args.out
        for k in range(NREG):
            mod = instrument(list(base), reg=k, alpha=args.alpha)
            outp = f"{prefix}_reg{k}{'_a' if args.alpha else ''}.spv"
            asm(mod, outp)
            print(f"reg{k} -> {outp}")
        return 0
    if args.ssa is None and args.reg is None and args.scalar is None:
        raise SystemExit("specify --reg K, --ssa ID, --scalar ID, or --step")
    mod = instrument(base, reg=args.reg, ssa=args.ssa, alpha=args.alpha, scalar=args.scalar)
    asm(mod, args.out)
    what = (f"scalar {args.scalar}" if args.scalar is not None
            else f"registers[{args.reg}]" if args.reg is not None else f"ssa {args.ssa}")
    print(f"{what}{' alpha' if args.alpha else ''} -> {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
