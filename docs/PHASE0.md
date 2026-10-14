# Phase 0 findings

Dump: Yo-kai Watch 2: Psychic Specters (USA), program ID `00040000001B2700`, product code `CTR-P-BYSE`.

## Code

| Segment | Address | Size |
|---|---|---|
| .text | `0x00100000` | `0x5F30D4` (~6.0 MiB, roughly 1.5M instructions) |
| .rodata | `0x006F4000` | `0x118274` |
| .data | `0x0080D000` | `0x053914` |
| .bss | after .data | `0x0BB0A8` |

- `.code` is BLZ-compressed. Decompressed size matches the exheader's segment span exactly, and the
  first instructions are a sane startup sequence (a chain of `bl` calls, one `blx` into Thumb).
- **No CRO/CRS/CRR files.** All game code is in the one static executable, so the recompiler doesn't need
  to handle dynamically loaded modules. (`ldr:ro` appears as a string, but it's just the SDK linking it.)
- **Mixed ARM and Thumb.** Around 3,500 `blx #imm` call sites switch into Thumb, so function discovery
  has to track instruction-set mode.
- Linear ARM-mode disassembly: around 10,800 `bx`/`blx` through a register and around 1,700 other writes to
  `pc`. These are returns, virtual calls and jump tables. Resolving the non-return ones is the main
  recompiler challenge. Thumb regions inflate these counts, so they're upper bounds.
- SVCs seen in ARM code (Thumb code not yet scanned): `0x32` SendSyncRequest (~350 sites), `0x23`
  CloseHandle, `0x24` WaitSynchronization1, `0x28` GetSystemTick, `0x18`/`0x19` Signal/ClearEvent,
  thread creation/priority, memory blocks, resource limits. A small, standard set.

## Services referenced

| Group | Services | Plan |
|---|---|---|
| Core | `fs:USER`, `hid:USER`, `gsp::Gpu`, `gsp::Lcd`, `dsp::DSP`, `cfg:u/s/i`, `ptm:*`, `ndm:u`, `err:f`, `am:app` | Implement natively |
| Camera | `cam:u` | Used for scanning QR codes/medals. Stub first; maybe a webcam or image-file picker later |
| Online / StreetPass / DLC | `http:C`, `ssl:C`, `soc:U`, `boss:*`, `frd:u`, `nim:aoc`, `cecd:*` | Stub as "offline / no data" |

## RomFS (2.6 GiB, 3,755 files)

| Content | Format | Plan |
|---|---|---|
| `yw2_a.fa` (1.1 GiB) and three `yw2_lg_*.fa` language archives (~129 MiB each) | Level-5 `ARC0` archive | Leave packed: the game reads them through its own code, which gets recompiled |
| 3,678 `.bcstm` + 3 `.bcsar` | Nintendo standard streamed/sound-archive audio | Native decoder (DSP-ADPCM/PCM) in the audio layer |
| 70 `.moflex` cutscenes | Mobiclip video | Decode with FFmpeg (has moflex demux + mobiclip decoder) |

## Next

1. Scan Thumb regions for SVCs and service calls to finish the list.
2. Start the recompiler: ELF-less loader for `code.bin`, ARM/Thumb decoder, function discovery from the
   entry point plus call targets.
