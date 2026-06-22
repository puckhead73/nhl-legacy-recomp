# NHL 12 `default.xex` — Binary Analysis Notes

*Source: `extracted/default.xex` from `NHL 12 (USA, Europe) (En,Fr,De,Sv,Fi,Ru,Cs).iso`
(XGD3, game partition @ `0xFD90000`). Parsed with `tools/xexinfo.py` (plaintext XEX2
headers only — basefile is encrypted+LZX, decryption handled by XenonRecomp tooling).*

## Identity

| Field | Value | Notes |
|---|---|---|
| Original PE name | **`nhlzf.exe`** | EA Canada internal name for the NHL12 executable |
| Title ID | `0x45410964` (`EA` / 2404) | NHL 12 retail |
| Media ID | `0x41AF51B7` | pin in `nhl12.toml` |
| Version / base | `0x00000004` / `0x00000004` | disc baseline; TU not yet applied (see TODO) |
| Regions | `0xFFFFFFFF` (region-free) | |
| Import lib version | 2.0.**20500**.0 | kernel build the title was linked against |

## Memory layout

| Field | Value |
|---|---|
| Image base / load address | `0x82000000` |
| Image size | `0x01A10000` (27,328,512) |
| Entry point | `0x828588A8` |
| Export table | `0x836D83EC` — title *exports* symbols (unusual; likely EA module/DLL system — investigate) |
| Default stack | 256 KiB |
| Title workspace | `0xB0000` |
| TLS info | present (`0x00020104` @ `0x2AD0`) — recompiled code needs TLS slot emulation via ReXGlue |

## Basefile format

- **Encryption:** retail key (encrypted file key `5e50b480c4628a144fa367ee3fe0badb`)
- **Compression:** LZX, window `0x8000`, first block `0xE800`
- ⇒ All code-level analysis requires the decrypted/decompressed basefile.
  XenonRecomp/XenonAnalyse handle this internally (tiny-AES-c + libmspack).
  For Ghidra, produce a decrypted image once and pin its hash.

## Imports (runtime work estimate, headline numbers)

| Library | Import records | Notes |
|---|---|---|
| `xboxkrnl.exe` | 387 | kernel: Ke/Mm/Nt/Ex/Vd/Xex families |
| `xam.xex` | 234 | user/profile/UI/net (XamUser, XamShow, XNet/WSA live here) |
| **Total** | **621** | function imports use 2 records (thunk+addr) ⇒ ~310–400 unique imports expected |

Only these two libraries — no xbdm/xapi. Per-ordinal disposition list (implement /
stub / override) requires the decrypted image: the import name table is plaintext but
ordinal records live in the image. → produced after first XenonRecomp/XenonAnalyse run.

## Optional headers present

`RESOURCE_INFO`, `FILE_FORMAT_INFO`, `ENTRY_POINT`, `IMAGE_BASE_ADDRESS`,
`IMPORT_LIBRARIES`, `CHECKSUM_TIMESTAMP`, `ORIGINAL_PE_NAME`, `STATIC_LIBRARIES`,
`TLS_INFO`, `DEFAULT_STACK_SIZE`, `SYSTEM_FLAGS` (`0x2600`), `EXECUTION_INFO`,
`TITLE_WORKSPACE_SIZE`, `GAME_RATINGS`, `LAN_KEY`, `XBOX360_LOGO`,
`ALTERNATE_TITLE_IDS`.

`STATIC_LIBRARIES` (@`0x298C`, ~0x144 bytes) will enumerate the EA/MS static libs and
their versions (XAPILIB, D3DX, XACT, EA internal libs) — decode in a follow-up; it's a
cheap, high-value fingerprint of the middleware stack.

## TODO / open questions

- [ ] Decode `STATIC_LIBRARIES` blob → middleware inventory (xexinfo.py extension)
- [ ] Locate + apply latest Title Update (`.xexp`) — plan §Phase 1.2; current baseline is disc v4
- [ ] Why does a title export a table at `0x836D83EC`? (EA runtime module system?)
- [ ] Pin SHA-256 of `default.xex` in `config/nhl12.toml` once TU decision is made
- [ ] Confirm `ALTERNATE_TITLE_IDS` contents (probably NHL 11/old-gen save import)
