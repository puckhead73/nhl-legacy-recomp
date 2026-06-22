# Magic Values & Hardcoded Constants

A reference of the specific numbers, addresses, and signatures that matter. All
CONFIRMED unless noted. Cross-references in [`gotchas.md`](gotchas.md).

## Identity
| Value | Meaning |
|---|---|
| `0x45410964` | NHL 12 **title ID** (also the shader-cache filename prefix `45410964.*`). |
| `nhlzf.exe` | the executable's internal name. |
| SHA256 `9950CD88…EB51` | the pinned `default.xex` hash. |

## Memory / image layout
| Value | Meaning |
|---|---|
| `0x82000000` | image base (guest). |
| `0x828588A8` | entry point. |
| 27.3 MB | image size. |
| 4 GiB | guest address space the recomp reserves (`host = base + addr`). |
| 512 MB | real 360 unified RAM. |

## Pinned code addresses
| Address | Meaning |
|---|---|
| `0x83366050` | `setjmp` (jmp_buf @ r3: f14-f31, r13-r31, CR, LR, v64-v127). |
| `0x833643B0` | `longjmp`. |
| `0x826FBD30` | `sub_826FBD30` — bring-up crash site (uninit object via `[r3+16]`→`lwz r10,36(r11)`). |
| `0x82A72C68`(+0x80), `0x82F313F8`(+0x14), `0x82F14648`(+0x64), `0x82C1F5E8`(+8), `0x82C1F604`(+8) | manual function boundaries (analyzer-missed). |
| `0x8268F750` | proven a function (95 `bl`-callers). |

## CRT/millicode noise (filter in crash traces)
| Range | Meaning |
|---|---|
| `0x8285Cxxx` / `0x8285Dxxx` / `0x8285Exxx` | EA CRT millicode. |
| `0x8336xxxx` | `__savegprlr_*` / `__restgprlr_*` save/restore helpers. |

## Recomp counts (the "size of the job")
| Value | Meaning |
|---|---|
| 103,714 | recompiled functions. |
| 154 | generated `nhl12_recomp.N.cpp` TUs (Phase 4). |
| 593 | XenonRecomp TUs (Phase 2, superseded). |
| 505 / 14,718 | jump tables / labels recovered. |
| 860 | gap (indirect-only) functions. |
| 6,681 | missing-instruction errors fixed (~50 emitters). |
| 317 | unique imports (200 xboxkrnl + 117 xam). |
| 304 / 13 | import function thunks / data-variable slots. |
| 101 | manual function spans (Phase 2). |
| 19,058 | import calls deep before Phase 2 stub build exited. |

## Import dispositions (counts)
`NetDll`×47 (stub) · `Ke/Ki/Kf`×34 (rt) · `Nt`×32 (rt) · `Vd`×25 (override) ·
`Rtl`×19 (rt) · `XMA`×17 (override) · `XamShow`×15 (stub) · `XeCrypt/XeKeys`×15 ·
`XamUser`×11 · `Ex`×10 · `XamInput`×4 (override). Full list: `docs/kernel_imports.csv`.

## Archive signatures
| Bytes | Format |
|---|---|
| `45 42 00 03` | **BigEB v3** ("EB", ver 3) — main `.big` containers. BE TOC. |
| `42 49 47 34` | **BIG4** ("BIG4") — tiny configs (`gps.big`, `options.viv`). |
| `0x400` | BigEB header alignment field. |
| counts: data0=2, boot=0x228, cache=0x3282 | observed BigEB entry counts. |

## Renderer
| Value | Meaning |
|---|---|
| `render_target_path_d3d12=rov` | the mandatory 3D path (RTV → black scene). |
| `DXRO` / `DXRT` | PSO file magics (ROV / RTV). |
| `45410964.xsh` | guest shader storage. |
| `45410964.{rov,rtv}.d3d12.xpso` | translated PSO storage (path-split). |
| `<cache_root>\shaders\shareable` | persistent shader storage dir. |

## Audio
| Value | Meaning |
|---|---|
| XMA register `0601` | what the XMA worker busy-spins on when music is missing. |
| `.sbr`/`.csi`/`.irf` | sound bank / cue script / reverb impulse. |
| `NA_En` | locale tag (North America, English). |

## Locales (disc)
`En, Fr, De, Sv, Fi, Ru, Cs` (English, French, German, Swedish, Finnish, Russian,
Czech).

See [`gotchas.md`](gotchas.md) and [`../unknowns/open-questions.md`](../unknowns/open-questions.md).
