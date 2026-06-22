# Function Index — Navigating 103,714 Functions

There is no symbol table. Every function is `sub_<hexaddr>`. This doc is the
**method** for finding your way around, plus the handful of addresses pinned so far.

## 1. Layout facts (CONFIRMED)
| Fact | Value |
|---|---|
| Image base | `0x82000000` |
| Entry point | `0x828588A8` |
| Image size | 27.3 MB (code + data) |
| Function count | **103,714** (`PPCFuncMappings` in `app/generated/default/nhl12_init.cpp`) |
| Generated TUs | 154 × `nhl12_recomp.N.cpp` (partitioned by address, not module) |
| Guest→host | host addr = `base + (uint32_t)guest_addr` |

The authoritative function list is **`app/generated/default/nhl12_init.cpp`**
(`PPCFuncMappings[] = { {0x82580000, sub_82580000}, … }`) — address → function pointer,
sorted by address.

## 2. Find the code for an address
1. `grep -rl "sub_<ADDR>" app/generated/default/` → the `nhl12_recomp.N.cpp` file.
2. Open it; the body is the translated PPC with original mnemonics as comments.
3. Callees are `sub_<target>` calls; for indirect/`bctr` see the switch-table config.

## 3. Find the address for an original module (the standing method)
Original `.cpp` modules compile to contiguous-ish address ranges. To pin a `[P4]`
module to addresses:
1. Find its **assert string** in the image (e.g. `…\ai\goalie\goaliesave.cpp`).
2. Find the function that references that string's address (a `lis`/`ori` pair loading
   the `.rdata` address) — that function is *in* `goaliesave.cpp`.
3. Neighbours in the address space are the rest of that module.
See [`codebase-map.md`](codebase-map.md) §6.

## 4. Find callers/callees of an address
Scan `extracted/nhlzf_image.bin` for the branch encodings to/from the address
(big-endian `bl`/`b` with the relative displacement). `tools/add_unresolved.py` and
`tools/find_gap_functions.py` contain the branch-decode pattern to reuse.

## 5. Pinned addresses so far (CONFIRMED)
| Address | What | Source |
|---|---|---|
| `0x82000000` | image base | XEX |
| `0x828588A8` | entry point | XEX |
| `0x83366050` | **`setjmp`** (saves f14-f31/r13-r31/CR/LR/v64-v127 to jmp_buf@r3) | disasm `[ASM]` |
| `0x833643B0` | **`longjmp`** | disasm `[ASM]` |
| `0x826FBD30` | `sub_826FBD30` — an early crash site during bring-up (uninit object via `[r3+16]`) | `[RT]` |
| `0x82A72C68` (sz 0x80) | manual function boundary (analyzer-missed leaf) | fixups |
| `0x82F313F8` (sz 0x14) | manual function boundary | fixups |
| `0x82F14648` (sz 0x64) | manual function boundary | fixups |
| `0x82C1F5E8` / `0x82C1F604` (sz 8 each) | jump-table shared-tail leaves | fixups |
| `0x8268F750` | proven a function by 95 `bl`-callers (image scan) | `[ASM]` |

These live in `app/nhl12_fixups.toml`. As subsystems get pinned, add their entry
addresses here.

## 6. Function classes to recognize (CONFIRMED)
- **CRT/millicode noise** to filter in crash traces: `0x8285Cxxx`/`8285Dxxx`/`8285Exxx`
  and `0x8336xxxx` save/restore helpers (`__savegprlr_*`/`__restgprlr_*`).
- **505 switch tables / 14,718 labels** — `bctr`-dispatched (`config/switch_tables.toml`).
- **860 gap functions** — indirect-only (`app/nhl12_gapfuncs.toml`).
- **Import thunks** — 304 `__imp__<Name>` provided by the runtime (not recompiled).

## 7. Next pins to capture (open task)
The high-value anchors to pin next, by module (see [`codebase-map.md`](codebase-map.md)):
`ai/aistruct` (AI state struct), `physics/physmodule` + `puck`, `rules/rulebook`,
`goalie/goalie`, `anim/animplayer`, the **NHL Sim** and **NHL Render** thread entries.

See [`global-state.md`](global-state.md) for data/globals and
[`../unknowns/open-questions.md`](../unknowns/open-questions.md).
