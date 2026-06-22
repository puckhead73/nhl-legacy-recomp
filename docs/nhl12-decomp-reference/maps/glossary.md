# Glossary — Recovered Names & Project Jargon

Definitions for names recovered from the binary (`[P4]`/`[RTTI]`/`[STR]`) and
terms used throughout this documentation. Where a name is an EA/RenderWare term of
art, that is noted. Confidence: names from `[P4]`/`[RTTI]` are **CONFIRMED** to
exist; their *meaning* is **INFERRED** unless stated otherwise.

## Engine / middleware

| Term | Meaning |
|---|---|
| **RenderWare 4 / rw4** | EA's in-house graphics+runtime kernel (post-Criterion). The `rw::` namespace. Provides filesystem, input, math, audio core, movie playback, codecs. The engine NHL 12 is built on. `[RTTI]` |
| **BaseKit** | EA's shared platform-technology collection (`packages/basekit/*`): memory, audio, online. Versioned per subsystem. `[P4]` |
| **EASTL** | EA's reimplementation of the C++ STL, tuned for games/consoles. `eastl::` namespace. `[RTTI]` |
| **MemoryFramework** | BaseKit allocator framework (v1.10). Categorised allocation (memory budgets per subsystem). `[P4]` |
| **RWAudioCore** | RenderWare Audio Core (v6.02) — the audio engine implementation behind `rw::audio::core`. Bundles a Speex decoder. `[P4]` |
| **Lynx** | Third-party/cross-EA middleware (v1.7.1) providing a **parameter/tuning registry** (`Attributed`, `HardBlend`) and a **particle system** (`ParticleEffect`, `ParticleSet`). `[P4]` |
| **OSDK** | EA Online SDK (v6.01) — settings, presence, download streaming. `[P4]` |
| **Blaze / BlazeSDK** | EA's online backend client (v3.09): matchmaking, rooms, playgroups, leaderboards, VoIP. `[P4]` |
| **EAStore** | EA's in-game store / DLC / entitlement system (v1.11). `[P4]` |
| **AdManager** | EA in-game advertising client (v3.06). `[P4]` |
| **XDK** | Xbox 360 Development Kit. This build links the **Feb 2011** XDK, incl. the D3D shader-microcode compiler. `[P4]` |

## Game packages (`internal/*`)

| Term | Meaning |
|---|---|
| **cmn** | The `internal/nhl` package's source module — the **common/shared game runtime** that bridges frontend and gameplay. `[P4]` |
| **nhlgameplay** | The on-ice simulation package: AI, physics, animation glue, rulebook. `[P4]` |
| **nhlfrontend** | Menus, game modes, **league logic** (GM Mode/franchise, injury generation), resource kernel. `[P4]` |
| **nhlrender** | NHL-specific rendering glue (particle render actions, scene/material binding) atop RenderWare. `[P4]` |
| **ResourceKernel** | nhlfrontend's resource/asset lifetime manager for UI. `[P4]` |
| **GM Mode** | "Be A GM" franchise mode. Data model in `leaguelogic/gmmodedata.cpp`. `[P4]` |

## Gameplay terms (from `nhlgameplay/ai/*` filenames)

| Term | Meaning |
|---|---|
| **CheckingStateMachine** | State machine for a body check: approach → contact → result/animation. `[P4]` |
| **ccheck / cstickcheck** | Body-check and stick-/poke-check logic modules. `[P4]` |
| **choreoglide** | "Choreography glide" — blended/scripted canned positional movement (e.g. lining up for a faceoff, smooth repositioning). `[P4]` |
| **PostWhistleBrain** | Behaviour controller for after the whistle (scrums, escorting, returning to position). `[P4]` |
| **twoplayeranim** | Two-player interaction animations (checks, board battles, ties) — couples gameplay state to paired animations. `[P4]` |
| **livefaceoff / tface / faceoffdrill** | The interactive faceoff system and its tutorial drill. `[P4]` |
| **stratoff / toffplay** | "Strategy-offense" / team-offense play execution (the rush, offensive systems). `[P4]` |
| **SaveSpace** | Goalie net-coverage geometry model — the area the goalie tries to fill. `[P4]` |
| **GoaliePose** | Goalie stance selection (butterfly/stand-up/post). `[P4]` |
| **LooseStickManager** | Tracks dropped sticks on the ice as dynamic items. `[P4]` |
| **WaterBottle** | Physics body for the bottle on top of the net (knocked off by high shots). `[P4]` |
| **randomd0** | A deterministic RNG source used by the sim (determinism matters for replays/netcode). `[P4]` |
| **NIS** | Non-Interactive Sequence — pre-scripted in-engine cinematic (the `nis.big` archive). `[STR]` |

## Recompilation / runtime terms

| Term | Meaning |
|---|---|
| **XenonRecomp** | The PPC→C++ ahead-of-time recompiler used in Phase 2. Hedge-dev project. |
| **RexGlue** | The Xenia-derived SDK (v0.8.0) adopted in Phase 4: own codegen + integrated runtime (kernel, memory, VFS, **D3D12/Vulkan GPU**, XMA audio). |
| **PPCContext** | The struct holding the emulated PowerPC register file (GPRs `r0..r31`, FPRs, VMX `v0..v127`, CR, XER, LR, CTR, FPSCR). Every recompiled function takes `(PPCContext& ctx, uint8_t* base)`. |
| **base** | Pointer to the start of the 4 GiB guest address space. Guest address `A` → host `base + A`. |
| **sub_<addr>** | A recompiled function, named only by its guest entry address (hex). The only naming scheme in the translated code. |
| **Switch table / jump table** | A `bctr`-dispatched branch table. EA's compiler interleaves them with nops so stock detectors miss them; 505 were recovered structurally. |
| **Gap function** | An indirect-only function (reached only via runtime-built pointers), invisible to static analysis; 860 recovered heuristically. |
| **setjmp/longjmp** | EA's engine wraps init in setjmp/longjmp; the recompiler must emit host `ppc_setjmp/ppc_longjmp` for the exact configured addresses or exception handling returns garbage. |
| **Vd\*** | Xbox 360 kernel "video" imports — the GPU ring-buffer boundary. Overridden, not recompiled. |
| **XMA** | Xbox 360 hardware audio codec. Decoder-context imports are MMIO-adjacent → overridden. |
| **EDRAM** | The 360's 10 MB embedded DRAM render-target memory; emulated by RexGlue's GPU. |
| **Xenos** | The Xbox 360 GPU (ATI). Its shaders are translated to DXBC/SPIR-V by RexGlue. |

## Asset / file terms

| Term | Meaning |
|---|---|
| **BigEB v3** | EA Canada's `.big` archive variant: magic `45 42 00 03` ("EB", ver 3), big-endian TOC. The disc's main containers. `[STR]` |
| **BIG4** | Classic EA BIG archive: magic `42 49 47 34`. Used by tiny configs (`gps.big`, `options.viv`). `[STR]` |
| **cache:\** | The VFS device the game reads ALL runtime assets from (expects a pre-populated 360-HDD install image). `[RT]` |
| **.rx2** | RenderWare asset chunk file (textures/skeletons). E.g. `rendering/boot/texlib_*.rx2`, `skeleton_bindpose/*.rx2`. `[RT]` |
| **.fxo** | Compiled shader object (`shaders/*.fxo`). `[RT]` |
| **AttribDB / renddb** | "Attribute database" (`AttribDB/renddb.{bin,vlt}`) — likely the material/render attribute database. `[RT]` |
| **.sbr / .csi** | Audio bank / cue-script files (`audio/*`). `[RT]` |
| **.irf** | Impulse-response file for reverb (`audio/reverb/*.irf`). `[RT]` |
| **scrape / scenedef.lua** | `cache:\scrape\{boot.scrape,scenedef.lua}` — a Lua-driven scene/boot definition. `[RT]` |
| **viv** | Tiny EA config archive (`options.viv`). `[STR]` |

## Documentation conventions
See [`../README.md`](../README.md) §3 for the full evidence-tag legend
(`[P4]`/`[RTTI]`/`[STR]`/`[IMP]`/`[RT]`/`[ASM]`/`[INV]`/`[INF]`) and the
**CONFIRMED / INFERRED / UNKNOWN** confidence markers.
