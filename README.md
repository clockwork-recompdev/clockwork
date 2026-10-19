# Clockwork

Native PC versions of the **Yo-kai Watch games on the Nintendo 3DS**, made by static
recompilation. Each game's ARM code is translated to C ahead of time and compiled to a normal
program; the 3DS's hardware and system services are replaced with native ones built on SDL3. It
is not an emulator: no interpreter, no JIT, no 3DS firmware. See
[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

This repository holds only tools and source code. It does not and will not contain any game code
or assets: you play from decrypted dumps of cartridges you own.

## Games

Every game is its own executable, built from one shared runtime when that game's recompiled code
is present. Versions that share code share an executable, and any executable's launcher hands a
dump to the one that runs it.

| Game | Versions | USA | Europe | Japan | Korea |
|---|---|---|---|---|---|
| Yo-kai Watch | | `yw1` | `yw1_eur` | `yw1_jpn` | `yw1_kor` |
| Yo-kai Watch 2 | Psychic Specters (Shin'uchi) | `yw2` | `yw2_eur` | `yw2_jpn` | `yw2_kor` |
| | Bony Spirits, Fleshy Souls (Ganso, Honke) | `yw2bf` | `yw2bf_eur` | `yw2bf_jpn` | `yw2bf_kor` |
| Yo-kai Watch Blasters (Busters) | Red Cat Corps, White Dog Squad | `blasters` | `blasters_eur` | `blasters_jpn` | `blasters_kor` |
| Yo-kai Watch 3 | Sushi, Tempura in Japan | `yw3` | `yw3_eur` | `yw3_jpn` | |
| | Sukiyaki | | | `yw3sk_jpn` | |
| Yo-kai Watch Busters 2 | Sword, Magnum | | | `busters2` | |

Each region's release is its own code, so it has its own executable. The console reports the
dump's region to the game; European releases are shown in the language picked on the Library tab.
The USA releases (and Busters 2) are the tested ones; the other regions are recognized and built
the same way but are new.

Psychic Specters is the most played and tested; the others are newer here.

## What it does

- **Launcher**: a game library with your title-screen images, dumps found automatically next to
  any one you pick, a version picker, and settings for display, controls and audio.
- **Graphics**: a GPU renderer on SDL_GPU (Vulkan; Metal and Direct3D 12 by design) with up to 4x
  internal resolution, a 16:9 widescreen top screen that shows more of the scene, adjustable HUD
  size, and one-screen layouts that follow what the game shows (map, menus, battles). A software
  renderer stays as a fallback. Frame interpolation (Graphics settings) shows the games' 30 fps
  at 60 by drawing an in-between frame for each one, at the cost of about two frames of delay.
- **Mods**: install from GameBanana in the launcher, or drop them in yourself. They're applied as
  the game starts and never change the dump: whole RomFS files (LayeredFS-style translations) and
  files inside Level-5's archives.
- **Saves**: each game keeps its own. Import a save from a 3DS (a Checkpoint or JKSM backup) on the
  Library tab; the current one is kept in `save-backups`.
- **Local wireless over the internet (Rooms)**: battles, trades and co-op between two windows on
  one PC, or with players anywhere through a relay. Pick a relay, a room and your name on the Rooms
  tab. The relay server lives in its own repository,
  [clockwork-relay](https://github.com/clockwork-recompdev/clockwork-relay), with a one-command install
  for a Linux server.

## Getting your dumps

Dump each cartridge with GodMode9 (`[C:] GAMECART` → *Dump cart (trimmed)*) and make sure it's
decrypted (*Decrypt file* if needed). Put the dumps in `dump/` (ignored by git).

## Building

Needs Rust, CMake 3.24+, a C/C++ compiler (Clang, GCC or MSVC), SDL3, libcurl and libarchive.

**Releases** are the easiest way: each one is the `clockwork` program (launcher and runtime, no
game), the tools, and what a game's code is compiled with. Add a dump and it builds that game as a
module in the data folder, with this PC's C compiler (Visual Studio's Build Tools on Windows, Xcode's
command line tools on macOS, gcc or clang on Linux); `clockwork` then plays it. Pushing a `v*` tag
makes one (`.github/workflows/release.yml`, `tools/package.sh`).

From source: build once (`cmake -B build -G Ninja && cmake --build build`), which makes
`clockwork` and the relay, plus any game whose code is generated. After that, **the launcher builds
games for you**: add a dump (Browse, or put it in the dump folder) and it unpacks, recompiles and compiles
that game's executable in the background, with progress on the Library tab and the tools' output in
`build.log` in the data folder. Adding another version of a game that shares an executable
rebuilds it with both.

By hand: unpack each dump, then recompile it to C into the folder its executable is built from.
For versions that share an executable, pass the other versions' unpacked folders too:

```sh
cargo run --release -p ctrx -- dump/psychic-specters.3ds extracted/yw2
cargo run --release -p recomp -- emit extracted/yw2 build/generated

cargo run --release -p ctrx -- dump/bony-spirits.3ds extracted/yw2-bony
cargo run --release -p ctrx -- dump/fleshy-souls.3ds extracted/yw2-fleshy
cargo run --release -p recomp -- emit extracted/yw2-bony build/generated-yw2bf extracted/yw2-fleshy

cmake -B build -G Ninja && cmake --build build
```

| Executable | Generated code goes in |
|---|---|
| `yw2` | `build/generated` |
| every other one (`yw2bf`, `yw2_eur`, ...) | `build/generated-<executable>` |

CMake builds whichever games have generated code (each folder is a cache variable, e.g.
`-DYW3_GENERATED_DIR=...`), plus `clockwork-relay`. `extracted/<game>/info.txt` summarizes a dump's
memory layout and the system modules it uses.

## Playing

Run any built executable (`build/yw2`, ...). The launcher opens; choose a dump and press Play.
`build/yw2 --play <dump>` skips the launcher; holding Shift at startup (or `--launcher`) brings it
back if you turned it off.

| | Keyboard | Controller |
|---|---|---|
| D-Pad | Arrows | D-Pad |
| Circle Pad | WASD | Left stick |
| A / B / X / Y | K / J / I / U | By position (east is A) |
| L / R, ZL / ZR | Q / E, 1 / 3 | Shoulders, triggers |
| Start / Select | Enter / Backspace | Start / Back |
| Touch | Click the bottom screen | |

Everything is rebindable on the Settings tab. In game: F5 changes the one-screen layout for the
current kind of scene, F6 adaptive layout, F7 widescreen, F8 picture-in-picture, F9 one or two
screens, Tab (or the right stick) swaps screens, F11 fullscreen. F12 saves a capture (screenshot
plus scene info) and Shift+F12 a 90-frame burst with draw logs, for bug reports.

Saves, settings, mods, title images and captures live in your user data folder:
`~/.local/share/yw2-recomp` on Linux, `~/Library/Application Support/yw2-recomp` on macOS,
`%APPDATA%\yw2-recomp` on Windows.

## Repository layout

| Path | Contents |
|---|---|
| `tools/ctrx` | Unpacks a decrypted dump: exheader, ExeFS, decompressed `code.bin`, RomFS |
| `tools/recomp` | The recompiler: ARM/Thumb decoder, function discovery, C emitter |
| `tools/check_decoder.py`, `tools/difftest.py` | Check the decoder against `llvm-objdump`, and emitted C against real ARM under `qemu-arm` |
| `runtime/` | The native runtime the generated code links against: kernel and HLE services, PICA200 renderer, DSP audio, launcher, mods, local wireless (`runtime/src/net`) |
| `docs/` | Architecture, plan and per-phase notes |
