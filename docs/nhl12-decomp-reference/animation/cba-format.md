# `.cba` Animation Bundle Format (NHL Legacy / NHL 12)

EA RenderWare-4 "GameData" stream. `data0.big` holds `gamedata/anim/anim.cba`
(~28 MB, the master skeletal-animation database) and `crowd.cba` (~1.1 MB).
Parser: [`tools/cba_parse.py`](../../../tools/cba_parse.py). All multi-byte fields
are **little-endian**.

## 1. Container

Three top-level chunks, each `[8-byte tag][u32 total_size incl. 16B header][u32 payload_size]`:

| Tag | Role |
|---|---|
| `GD.STRMl` | container; `total_size` ≈ whole file |
| `GD.REFLl` | reflection schema (struct/field-name vocabulary) |
| `GD.DATAl` | one per record; N records back-to-back. Next record = `offset + total_size` |

`anim.cba`: **46,664 records, 329 distinct types**. `crowd.cba`: 2,380 / 63.

## 2. GD.DATA record payload

```
+0x00  u32   0x030e6205        format magic (constant; marks every record/sub-record)
+0x04  12 B  0
+0x10  u32   type_hash         identifies the reflected struct type
+0x14  u32   pad (0)
+0x20  ...   field-descriptor table, then field data
```

Record **type-names are NOT in REFL** — they are authored strings inline in the
payload (the trailing `string` field). `cba_parse.py` labels each `type_hash`
empirically by the dominant authored name across its records.

### Reflected field table (at `+0x20`)
A run of 16-byte descriptors, each `(u32 count, u32 capacity, u32 data_offset, u32 0)`,
ending when the next quad isn't a valid `(…, …, off, 0)`. Each descriptor points at a
variable-length array elsewhere in the payload. **Element stride = (next field's
data_offset − this data_offset) / count** (the last field's span includes trailing
alignment pad, so detect strings by content, not stride). Fixed scalar fields (floats,
GUIDs) for some types live inline between the table and the array data.

## 3. Record categories (by `type_hash`)

Each type's name and field list are recovered from REFL (string just before
`__guid` = type name; strings after `__base` = field order). `cba_parse.py` prints
these. The animation-relevant types:

- **`DctAnimationAsset`** `0xf8ff03e3` (3,577 recs, ~12 MB — the data mass): the
  compressed keyframe tracks. Fields: `NumKeys, NumQuats, NumVec3, NumFloat,
  NumFloatVec, Cycle, QuantizeMultBlock, KeyTimes, QuantizeMultSubblock,
  CatchAllBitCount, DofTableDescBytes, DeltaBaseX/Y/Z/W, BitsPer…`. See §4.
- **`ClipControllerAsset`** `0x75e9ea49` (3,687, ~282 B): the per-clip header.
  Fields: `Target, Anims` (→ the DctAnimationAsset), `NumTicks, TickOffset, FPS,
  FPSScale, Distance` (root-motion distance).
- **`ActorControllerAsset`** `0xe08752b2` (`*.Player0`) & **`SequenceContainerAsset`**
  `0x90ff09e7` (`SEQ_NHL12_*`): montages/sequences that **embed nested sub-records**
  inline (each child begins with the `0x030e6205` magic — the format is recursive).
  Fields incl. `Length, NumTracks, Tracks, TrajectoryState`.
- **`TimeScaleBlendAsset`** `0xeeb4219b`: `WeightCurves, WeightCurvesOut, BlendWeight`.
- **Structural / tags**: `TagCollectionSet`, `BranchOutPointTag`/`BranchInWindow`
  (gameplay-cancel windows), `HIKStickTargetTag`/`HIKStickArcRangeSpecTag` (**HumanIK**
  stick targeting), `*GameState Tag`, `SfxSkating`/`SfxCollision`.

## 4. `DctAnimationAsset` record — decoded

Every field is serialized as a `(count, capacity, data_offset, 0)` array descriptor;
the scalar counts (`NumKeys` etc.) equal the lengths of their companion arrays.
Example `CHKP04_SP_SHFBF_3a` (a checking anim), `NumKeys = 43`:

| field | count | element | maps to |
|---|---|---|---|
| f@0x20 | 43 | byte | `KeyTimes` (one per key) |
| f@0x30 | 4176 | byte | **`DofTableDescBytes`** — the bit-packed DOF/curve stream (the bulk) |
| f@0x40–0x70 | 4 × int16[43] | int16 | `DeltaBaseX/Y/Z/W` per-key base tracks (quaternion components) |
| f@0x80 | 35 | int16 | `QuantizeMultSubblock` / bit-count table (sparse `0,2,3,5…`) |
| f@0x90 | 312 | int16 | quantized residuals — values ride ±32767 (±30860/32767 ≈ ±0.94 ⇒ unit-quaternion-scale) |
| f@0xa0 (last) | n | string | clip name |

So the compression is **delta + variable-bit-width quantization** (not a flat int16
dump): a base track (`DeltaBase*`) plus packed residuals scaled by
`QuantizeMultBlock`/`QuantizeMultSubblock`, with per-channel widths from `BitsPer…`
and an active-channel map in `DofTableDescBytes`.

## 5. Status & remaining work

- **DONE**: container parse; record enumeration/typing; **real engine type names +
  field schemas from REFL**; generic reflected-field decode (`--decode` / `--find`);
  nested-record detection; full identification of the compression model and its
  parameters.
- **TODO — reconstruct float poses**: implement the `DctAnimationAsset` decompressor
  (walk `DofTableDescBytes` to know active channels, unpack residuals at `BitsPer…`
  widths, dequantize with `QuantizeMult*`, add to `DeltaBase*`, normalize quaternions),
  then bind to the skeleton (`cache:\rendering\skeleton_bindpose\*.rx2`) and the per-clip
  `FPS`/`Distance` from `ClipControllerAsset`. **Recommended route**: rather than
  reverse the exact bit layout blind, **hook the runtime decompressor** in the recomp
  (the game decodes these to pose joints every frame) and capture the output
  matrices/quaternions — same runtime-capture approach already used elsewhere in this
  project, and self-validating. Static RE of the bit stream is the fallback.

## 6. Float poses — RUNTIME HOOK FAILED, HEADLESS-INVOKE chosen

**Runtime hook abandoned.** Statically-traced chain `sub_82CA9B20 → sub_82CA9618 →
sub_82CA7BD0` was hooked (gpu/hooks/anim_capture.cpp, REX_HOOK_RAW) but **never fires**
in real gameplay (proven: 1104 `[highcut]` control-hook hits, 0 `[anim]`). The chain
isn't the live per-frame path (recompiler likely inlined the vtable-dispatched evaluate,
or the live path is the higher-level blend-tree). The hook code is kept (env-gated,
pass-through) but is a dead end for this purpose.

**Chosen route: headless-invoke the real recompiled decoder** (the code exists even if
not called live — read it as the spec, run it as the impl). The decode functions are
SELF-CONTAINED (only local helpers, no kernel/imports) and read float constants from
the guest image `.rodata` (~`0x8208xxxx` + the cosine basis table @ **`0x83A413B0`**).

### Pipeline (per clip), as the game drives it
`sub_82CA7BD0` is the FINAL stage, not the whole decoder. Real sequence (verified by
reading the orchestrator `sub_82CA9618` @ recomp.70.cpp:64378, the reference for how
to drive it):
1. **Init descriptor D** from the asset: `sub_82CE3750(D, asset)` — pure computation,
   builds D's 64-byte-record-pool geometry from the asset's counts. NO allocator. Safe
   to call directly.
2. **Prime** each component group's container: `sub_82CA8B88(ctrl, asset)` then
   `sub_82CA8C60(ctrl, asset)` — these **allocate** the unpack scratch.
3. **Seek** to the frame: `sub_82CE1AC0/1AD8/1AF0(...)` then `sub_82CE3548(...)`
   (called once per the 3 component groups; gated on success each time).
4. **Decompress** `sub_82CA7BD0(ctrlScratch, out, weights)` ×3 — once per component
   group (quaternions / vec3 translations / floats). In the orchestrator each call uses
   a distinct scratch descriptor (`r1+128/+208/+224`), output buffer (`r1+96/+80/+112`),
   and weight scalar (`r1+192/+160/+144`, all init to a `.rodata` float @ `0x8208...`).

### ⚠️ CORRECTION — allocation needs a LIVE (booted) guest, not just `Runtime::Setup`
The prime step's real allocator is `sub_82CA8AE0` (recomp.70.cpp:62705). It does **not**
use a plain heap — it allocates through a **guest allocator object's vtable**:
`r3 = [ctrl+0]; allocFn = [[r3+0]+4]; bctrl allocFn(...)` (after `sub_83219240`, which
fetches the current allocator from a guest global `~0x8208xxxx`). That global is
populated by the **guest's own startup**, NOT by `Runtime::Setup`. So the headless
harness must **boot the guest to the menu first**, then invoke the decode on a worker
thread against the live allocator — exactly the `NHL_DUMP_*_RUNTIME` boot-then-scan
pattern in `nhllegacy_app.h::LaunchModule` (let the guest boot, wait `NHL_DUMP_DELAY_MS`,
run, hard-exit). The earlier "guest heap from `Runtime::Setup`" premise was wrong.

By contrast `sub_82CA7330` (recomp.70.cpp:59142) is a **size calculator**, not an
allocator (it returns the unpacked-form byte size from the asset's bit-packed DOF
table) — and `sub_82CE3750` is a pure descriptor builder. Both are **leaf functions**
(no `bl`, no `stwu`, no globals), so they validate the host→guest call harness with
zero live-guest dependency. `src/anim_decode.cpp` Milestone 0 calls exactly these.

### ⚠️ The `asset` is the RUNTIME (loaded) struct, NOT the on-disk record
The decode fns read asset fields at `+4 / +16 / +24 / +28 / +40 / +44 / +52 / +76 / +88`
(counts at +4/+16/+28/+44; a sub-array of `{ptr@+0, stride@+8}` descriptors at +24;
array pointers at +28/+40/+52/+76/+88). This is the layout the **GD loader produces in
guest RAM**, which is *not* byte-identical to the on-disk reflected
`(count,cap,offset,0)` descriptor table. Two ways to obtain a valid `asset`:
- **(A, preferred) Use the game's already-loaded asset.** The booted game loads
  `anim.cba` and relocates every `DctAnimationAsset` into this runtime layout. Resolve
  the target via the `ClipControllerAsset` (`0x75e9ea49`) → `Anims` field →
  `DctAnimationAsset` pointer (or scan guest RAM for the loaded record). No hand
  relocation, no layout guessing — the riskiest part disappears.
- **(B, fallback) Hand-build it** — requires reverse-engineering the GD-loader's
  on-disk→runtime transform first (the field offsets above are NOT the on-disk order).

### Host→guest call harness (proven recipe — `src/anim_decode.cpp`)
Recompiled fns are plain linkable symbols (`DECLARE_REX_FUNC` →
`void sub_XXXX(PPCContext&, uint8_t* base)`), callable from any TU in the `nhllegacy`
target that includes `generated/default/nhllegacy_init.h`. To call one from host code
after `Runtime::Setup`:
```cpp
auto* mem  = runtime()->memory();                 // rex::memory::Memory*
uint8_t* base = mem->virtual_membase();            // guest membase (host ptr)
uint32_t stk = mem->SystemHeapAlloc(64*1024, 0x1000);  // guest stack (virtual)
PPCContext ctx{};
ctx.fpscr.InitHost();
ctx.r1.u32 = (stk + 64*1024 - 0x100) & ~0xFu;      // stack top, 16B-aligned, headroom
ctx.r3.u32 = argA; ctx.r4.u32 = argB; ctx.r5.u32 = argC;  // PPC int args r3..r10
sub_82CE3750(ctx, base);                            // call; return value in ctx.r3.u32
```
Guest memory is **big-endian**: when seeding fields from host, write byteswapped
(`_byteswap_ulong`), matching how `REX_LOAD_U32` reads them. Allocate all guest-side
buffers (asset, D, outputs) with `mem->SystemHeapAlloc(size, align)` → guest VA; read
back via `mem->TranslateVirtual<T>(va)`.

### Structs to build in guest RAM
- **descriptor D** (`[controller+0]`, also consumed by `sub_82CE18E0`):
  `{+0 base→64-byte records, +4 rowCount, +8 rowStride, +12 NumKeys}`.
  `sub_82CE18E0(D,row,col)` → `D.base + ((D.stride*row+col) << 6)` (64-byte records).
  Build it by calling `sub_82CE3750(D, asset)` — don't hand-fill (it derives ~16 fields).
- **out4** = key-record output buffer, `NumKeys*64` bytes, addressed identically to D;
  each 64-byte record = **8 quantized int16 quaternions** (8×4×int16).
- **out5** = 12-byte `{float maxErr (init = large negative), u32 trackIdx, u32 keyIdx}`.

### Notes
- `sub_82CE1900` (delta-integrator over the 64-byte records) is NOT called by
  `sub_82CA7BD0`; it's a sibling used by the seek/prime path — int16 quaternion
  components are **delta-coded across keys** and prefix-summed.
- Output int16 quaternions → float = `v/32767` (unit quat), then map to bind-pose
  skeleton (`cache:\rendering\skeleton_bindpose\*.rx2`) + clip FPS/Distance from the
  `ClipControllerAsset` (`0x75e9ea49`).
- Function anchors (generated/default/nhllegacy_recomp.N.cpp; the `recomp.N` here ==
  `nhllegacy_recomp.N`): `sub_82CA7BD0` @ 70:60381, `sub_82CA7330` @ 70:59142 (size
  calc), `sub_82CA7390` @ 70:59198, `sub_82CA8AE0` @ 70:62705 (real allocator),
  `sub_82CA8B88` @ 70:62808, `sub_82CA8C60` @ 70:62940 (prime), `sub_82CA9618` @
  70:64378 (orchestrator — the drive reference), `sub_82CA9B20` @ 70:65128 (entry);
  helpers `sub_82CE3750` @ 73:18027 (descriptor-D builder), `sub_82CE18E0` @ 73:13344,
  `sub_82CE1900` @ 73:13362, `sub_82CE19F0` @ 73:13490.

### Status of the headless harness (`src/anim_decode.cpp`)
- **Milestone 0 — call harness proof ✅ DONE (2026-06-19, 17/17 PASS):** under
  `NHL_ANIM_DECODE=<out>`, build a synthetic asset in guest RAM, call the two leaf fns
  `sub_82CE3750` (descriptor builder) and `sub_82CA7330` (size calc), and assert the
  outputs against hand-derived arithmetic. Confirmed: a host-constructed `PPCContext`
  (`fpscr.InitHost()`, `r1` = `SystemHeapAlloc` stack top) + direct calls to the
  recompiled symbols reproduce the PPC arithmetic exactly. Run:
  `NHL_ANIM_DECODE=anim_decode_report.txt nhllegacy.exe --game_data_root <H:\...>`
  (fast-exits in `LaunchModule` before the game loop; needs game data + GPU for
  `Runtime::Setup`, but no booted guest). Report → `anim_decode_report.txt` next to exe.
- **Milestone 1a — recon (DONE 2026-06-19):** `NHL_ANIM_DECODE=scan` boots the guest
  then scans committed guest memory for resident GD records (magic `0x030e6205`, BOTH
  byte orders), histograms type_hashes, hexdumps `DctAnimationAsset` hits, and locates
  clip-name strings (`src/anim_decode.cpp::RunAnimScan`). **Menu run result:** 0 magic
  hits in 1.7 GB; the clip name `CHKP04_SP_SHFBF_3a` appears **only in the image**
  (`0x821DDA24` + its `0x92…` mirror) = compiled-in string constant, **not** loaded
  data. ⇒ **anim.cba is NOT resident at the menu — it loads on-ice** (matches the prior
  finding that the anim fns only fire in gameplay). So the scan must run in-game.
  Added **auto-trigger** `WaitForAnimData(vbase, timeout)` — polls every 3 s for the
  clip name appearing in **heap** (outside the image) and fires the full scan the
  moment anim.cba becomes resident, so the run no longer races a fixed delay.
  **NEXT (user):** run `NHL_ANIM_DECODE=scan`, start an on-ice game; scan auto-fires →
  `anim_scan_report.txt`. The histogram + hexdumps then decide route A: are loaded
  records still GD-format-in-place (magic + arrays), or deserialized into runtime
  objects reached via `ClipControllerAsset(0x75e9ea49)→Anims`? (NOTE: the decode fns
  read `asset+16` as a *count*, but on-disk `+16` = type_hash → the runtime `asset` is
  almost certainly a deserialized object, not the raw record.)
- **Milestone 1b — locate a loaded asset (next):** from the on-ice report, resolve a
  real `DctAnimationAsset` pointer (route A).
- **Milestone 2 — full pipeline:** prime → seek → `sub_82CA7BD0` ×3 → read int16 quats.
- **Milestone 3 — export:** int16→float (`v/32767`), normalize, map to bind-pose
  skeleton + per-clip FPS/Distance; scale to all 3,577 clips.

## CLI

```
python tools/cba_parse.py anim.cba                      # summary + type/clip census
python tools/cba_parse.py anim.cba --schema s.json      # types + REFL vocabulary
python tools/cba_parse.py anim.cba --records r.csv      # per-record manifest
python tools/cba_parse.py anim.cba --decode 1638        # decode one record's fields
python tools/cba_parse.py anim.cba --find CHKP04_SP     # decode by clip name
python tools/cba_parse.py anim.cba --names              # all authored names
```
