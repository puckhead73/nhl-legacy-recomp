# CZ Material Functions — Disassembly Analysis

Detailed RE of the color-zone equipment recolor functions. Disassembly is CONFIRMED
(from `objdump` on `extracted/nhlzf_image.bin`); struct-offset and role annotations
are INFERRED and marked. See [`README.md`](README.md) for the overview.

Objdump used:
```
RexGlue/tools/binutils/powerpc-none-elf-objdump.exe -D -b binary \
  -m powerpc:common64 -EB --adjust-vma=0x82000000 \
  --start-address=<A> --stop-address=<B> extracted/nhlzf_image.bin
```

---

## 1. `sub_82BB6CF8` — CZ material input-handle cache (FULLY DECODED)

**Signature (INFERRED):** `void sub_82BB6CF8(Material* this /*r3*/)`
**Size:** 0x110 (`0x82BB6CF8`–`0x82BB6E08`). **File:** `nhl12_recomp.52.cpp`.
**Called by:** `sub_82BBB670` (bl @ `0x82BBB688`), `sub_82BBD868` (bl @ `0x82BBD87C`)
— the two CZ material inits.

It resolves the equipment material's shader inputs **by name/id** and caches the
handles + slots into the material object. Annotated:

```
mflr r12; bl 0x8285c644 (__savegprlr); stwu r1,-128(r1)
r10 = [r3 + 0x20]            ; this->effect            (the RW effect/shader object)
r30 = 0x8211F23C            ; "diffuseSampler"   (lis 0x8212; addi -0xDB4 = 0x8211F24C
                            ;  → recolor_template1; r4 below uses r30-16 = diffuseSampler)
r31 = r3                    ; save this
r27 = -1                    ; INVALID_HANDLE sentinel

; --- resolve "diffuseSampler" ---
r11 = [r10 + 0x74]          ; effect->vtable_or_api   ([effect+116])
r3  = [r11 + 0x08]          ; api->getParamByName fn-or-obj ([+8])
if (r3 == 0) r3 = -1;       ; null api → INVALID
else r3 = call 0x82adcb38(r3, r4="diffuseSampler")   ; getParamByName
[this + 0x48] = r3          ; this->h_diffuse   (+72)

; --- resolve "recolor_template1" ---  (same pattern, r4 = r30 = "recolor_template1")
[this + 0x4C] = call 0x82adcb38(effect, "recolor_template1")   ; this->h_recolorTpl (+76)

; --- resolve handles → slots ---
r4 = [this + 0x48]          ; h_diffuse
r3 = call 0x82adc920(effect, h_diffuse); [this+0x50] = [r3+8]   ; this->slot_diffuse (+80)
r4 = [this + 0x4C]          ; h_recolorTpl
r3 = call 0x82adc920(effect, h_recolorTpl); [this+0x54] = [r3+8]; this->slot_recolorTpl (+84)

; --- bind 2 constant slots by id, loop 2x ---
r28 = this + 0x3C           ; &this->constHandles[0]   (+60)
r30 = 0x83758B60            ; &constIdTable[0]  (lis 0x8375; addi -0x74A0)
r29 = 2                     ; count
loop:
  r4 = [r30]                ; constId  (table = {0x000D0001, 0x000D0002})
  r11 = [this+0x20]; r11=[r11+0x74]; r3=[r11+8]    ; effect api
  if (r3==0) r3=-1; else r3 = call 0x82adcaa8(effect, constId)   ; getConstantById
  [r28 += 4] = r3           ; this->constHandles[i]   (+60, +64)
  r30 += 4; if(--r29) loop
addi r1,r1,128; b 0x8285c694 (__restgprlr)
```

### Material struct offsets recovered (INFERRED meanings)
| Offset | Field | Evidence |
|---|---|---|
| `+0x20` (32) | `effect` (RW shader/effect object) | base for all param lookups |
| `+0x3C` (60) | `constHandle[0]` (from id `0x000D0001`) | loop store |
| `+0x40` (64) | `constHandle[1]` (from id `0x000D0002`) | loop store |
| `+0x48` (72) | `h_diffuse` (handle of `diffuseSampler`) | store after lookup |
| `+0x4C` (76) | `h_recolorTpl` (handle of `recolor_template1`) | store after lookup |
| `+0x50` (80) | `slot_diffuse` (descriptor slot/index) | `[resolve()+8]` |
| `+0x54` (84) | `slot_recolorTpl` | `[resolve()+8]` |
| `effect+0x74` | api/vtable ptr; `[+8]` = getParamByName-ish | indirected each call |

> **So the equipment material binds, at minimum: `diffuseSampler` (the `_dm` base map),
> `recolor_template1` (the zone mask), and 2 recolor constants (`0xD0001`,`0xD0002` —
> almost certainly the `RecolorCBuffer` zone-colour array + tint).** These cached
> handles/slots are what the per-frame draw uses to bind textures and set constants.

### Why this function matters to the bug
It is the **wiring point** for the two prime speckle suspects: the `recolor_template1`
texture binding (`+0x4C/+0x54`) and the recolor constants (`+0x3C/+0x40`). If a later
draw uses `slot_recolorTpl` to bind the template at a wrong fetch/sampler state, or the
constants are filled with garbage, the speckle follows. Breakpoint here to capture the
live handles for a broken pad. (It does **not** itself corrupt data — it only caches
handles — so it is the *map*, not necessarily the *fault*.)

---

## 2. `sub_82BAB7F0` — `equipment_cz.fxo` effect build (HEAD DECODED)

**File:** `nhl12_recomp.52.cpp`. References the `equipment_cz.fxo` string at `+0x134`
(`0x82BAB924`). Builds/loads the cz effect. Head:

```
stwu r1,-528(r1); std r5,560(r1)
r30 = r4; r24 = r6; r25 = [r1+566] (byte arg)
r28 = call 0x82b5f720(r25)           ; lookup/begin (effect registry?)
if (call 0x82b65118() == 0) goto fail
if ([r1+563] (byte) != 12) goto fail ; <-- material KIND == 12 gate (the "12" seen elsewhere)
... call 0x82b794f8, 0x82b5f780, 0x82b650a8, 0x82b62668 ...
r3 = &local(0x90); call 0x82b810f0   ; construct effect desc
zero locals [+80..+96]
call 0x82b78860 ...                  ; (continues: loads "equipment_cz.fxo" at 0x82BAB924)
```

INFERRED: this is the constructor/loader for the equipment color-zone effect — it
gates on material **kind == 12** (note: prior renderer notes saw a `==12` kind check in
the *fetch* path too; worth correlating), builds an effect descriptor, and loads
`shaders/equipment_cz.fxo`. The sibling functions `sub_82BABA80`, `sub_82BABE58`,
`sub_82BAC4A0` each load a different `equipment_cz.fxo` variant (diffuse / no_ao /
nospotlight permutations).

---

## 2b. The CZ vtable + the Render method (CONFIRMED)

`sub_82BBB670`/`sub_82BBD868` have **zero `bl` callers** → they are **virtual methods**.
The `CZRenderable` vtable is at **`0x8211FB54`** (a second descriptor/message table is at
`0x825173C0`). Dumping it maps the whole class:

| vtable slot | function | size | role (INFERRED) |
|---|---|---|---|
| `0x8211FB50` | `sub_82BBD868` | — | init #2 (caches handles) |
| `0x8211FB54` | `sub_82BBB670` | 0x108 | init #1 (zero-fills `this+172`, caches handles) |
| `0x8211FB58` | `sub_82BB6F88` | 0x58 | small accessor |
| `0x8211FB68` | `sub_82BBBEF8` | 0x48 | getter/forwarder (shared w/ other vtable) |
| `0x8211FB8C` | **`sub_82BB98B8`** | **0x410** | **Render/Apply — the per-frame draw** |
| … | `sub_829FD7D0` | — | appears 3× = pure-virtual/no-op stub |

### `sub_82BB98B8` — CZ Render/Apply (the per-frame consumer) — CONFIRMED
**Signature (INFERRED):** `void Render(this /*r3=r20*/, RenderCtx* /*r4=r19*/)`. Structure:
- reads `ctx[+560]`, a global flag at `0x8388_84A0+0x129`;
- iterates the renderable's **sub-mesh list** at `this+0x38` (56) (`lwzx r25,[r11+r23]`),
  and per sub-mesh reads material/flags (`[r25+0xB0]`, `[r25+0x19]`), and the cached
  fields via `this+20/18`-indexed tables;
- per draw it calls the **bind/draw API**: `0x82ae5780` ×4, `0x82ae4dc8` ×4,
  `0x82ae2900` ×4 (the **4 material maps** → diffuse / recolor_template1 / normal /
  shine), `0x82adc908` ×2 (resolve/set), `0x82b284f0` ×2 (draw/submit).
- **No `lfs/stfs/vpk/vcfp` (zero float/vector ops).** ⇒ the recolour **colours are NOT
  packed here**; they are set as **precomputed parameters** (the cbuffer is filled
  elsewhere and bound by handle). So a *cbuffer-fill* miscompile would corrupt *solid
  zone colours*, not the *per-texel speckle* — the speckle is a **per-texel input**
  (a sampled map) or **shader math**, consistent with the texture-binding API being the
  hot path here.

> **Conclusion from the render method:** the per-texel speckle is fed by one of the 4
> maps bound in `sub_82BB98B8` (diffuse / template / **normal(DXN)** / shine) or by the
> `equipment_cz.fxo` shader. It is *not* the RecolorCBuffer colour constant. This
> narrows the fix to the texture-binding/sampling or shader-translation layer — see the
> Fix Playbook in [`README.md`](README.md) §4.

## 3. Call graph (CONFIRMED edges)

```
sub_82BBB670 ─┐  (CZRenderable init; zero-fills this+172, then:)
              ├─► sub_82BB6CF8  ── caches handles ──► 0x82adcb38 (getParamByName)
sub_82BBD868 ─┘  (CZHelmetJersey init)             ├─► 0x82adc920  (resolveHandle→slot)
                                                   └─► 0x82adcaa8  (getConstById)

sub_82BAB7F0 / A80 / E58 / C4A0  ── build equipment_cz.fxo effect ──►
   0x82b5f720, 0x82b65118, 0x82b794f8, 0x82b810f0, 0x82b78860, … (RW effect API)
```

The **per-frame draw** + **`RecolorCBuffer` fill** are reached from the callers of the
inits `sub_82BBB670`/`sub_82BBD868` (the renderable's render path) — the next pin
target. `sub_82BBB670` itself takes `(this /*r3*/, flag /*r4*/)`, zero-fills 24 bytes
at `this+172` (a 6-slot table — note this matches the 6 cached handle/slot fields), and
calls `sub_82BB6CF8`.

## 4. Useful snippets (for Codex)

**bl callers of an address** (find who calls a function):
```python
import struct
data=open('extracted/nhlzf_image.bin','rb').read(); BASE=0x82000000
def bl_callers(target):
    out=[]
    for off in range(0,len(data)-4,4):
        w=struct.unpack_from('>I',data,off)[0]
        if (w>>26)==18 and (w&1)==1 and (w&2)==0:      # bl, LK=1, AA=0
            d=w&0x03FFFFFC
            if d&0x02000000: d-=0x04000000             # sign-extend 24-bit
            if BASE+off+d==target: out.append(BASE+off)
    return out
```

**VA → containing `sub_`** (bisect `PPCFuncMappings` in `nhl12_init.cpp`): parse all
`{ 0x........, sub_ }` addresses, sort, `bisect_right - 1`.

## 5. Confidence
- **CONFIRMED:** all addresses, the disassembly, the call edges, the cached
  inputs (`diffuseSampler`, `recolor_template1`, const ids `0xD0001/0xD0002`), the
  struct store offsets, the `kind==12` gate.
- **INFERRED:** the field *names/meanings*, the class identities
  (`CZRenderable`/`CZHelmetJerseyRenderable`), and that this path carries the speckle.
  Confirm via the breakpoint+dump plan in [`README.md`](README.md) §4.
