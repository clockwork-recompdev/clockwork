# Architecture

## Goal: a native port, not an emulator

The game's code is translated **ahead of time** into C, which a normal compiler (clang/MSVC) turns into
native x86-64 or arm64 machine code for Windows, macOS and Linux. The 3DS hardware and OS it talked to
are replaced with native implementations built on SDL3.

What this rules out, and what we do instead:

| An emulator would... | This project instead... |
|---|---|
| Interpret or JIT-compile ARM instructions while the game runs | Translates every game function to C at build time. **No interpreter or JIT ships in any build.** If the recompiler misses a function, that's a recompiler bug to fix, not something to fall back on at runtime. |
| Simulate the 3DS kernel and system modules (running Nintendo's firmware code) | Implements the services the game calls (files, input, time, threads, sync) as ordinary native functions. No 3DS firmware or system modules are needed. |
| Execute PICA200 GPU command buffers on a virtual GPU, compiling shaders at runtime | Translates the game's vertex shaders to native shader code **at build time**, generates the fragment shaders for the fixed-function color combiner from the states the game uses, and maps draws to SDL_GPU (Vulkan, Metal or Direct3D 12). Where it pays off, hooks the game's own rendering calls directly instead of decoding command buffers. |
| Emulate the DSP | Implements audio playback natively (HLE): decodes the game's sound data and mixes it on the CPU through SDL audio. |
| Lock the game to 400×240 at 30 fps on a fixed CPU clock | Renders at any resolution. Game timing is the game's own logic, so framerate and widescreen become patches to the recompiled code. |

### What does carry over from the 3DS

- **The game's 32-bit memory layout.** Game data is full of 32-bit pointers, so the recompiled code
  addresses a 4 GiB reserved block through a base pointer (`base + guest_addr`). That's a data layout
  choice, not emulation: the code accessing it is native. N64Recomp and XenonRecomp do the same.
- **Game-visible behavior of services.** Our native file service still has to return what the game
  expects, so we follow 3dbrew and Azahar for *behavior*, not for structure.

## Cross-platform rules

| Concern | Rule |
|---|---|
| Targets | Windows x64, macOS arm64 (Apple Silicon), Linux x64. CI builds all three on every push. |
| Graphics | SDL_GPU only: Vulkan on Linux and Windows, Metal on macOS, D3D12 on Windows. No direct Vulkan/OpenGL calls, and no MoltenVK. |
| Shaders | Generated as SPIR-V at build time and cross-compiled to MSL (Metal) and DXIL (D3D12) with SDL_shadercross. |
| Windowing, input, audio, threads | SDL3. No platform-specific APIs outside a small `platform/` layer. |
| Generated code | Portable C17: no inline asm, no x86 intrinsics, no GCC-only extensions. It must compile with clang (Linux and macOS) and both clang-cl and MSVC (Windows). |
| Endianness | All targets and the 3DS are little-endian, so guest memory is used as-is. |
| Build | CMake for the runtime and generated code; Cargo for the tools. |

## Pipeline

```
your dump ──ctrx──▶ code.bin + CROs + RomFS
                        │
                        ▼
                 recompiler (Rust) ──▶ generated/*.c   (one C function per game function)
                        │
shaders in code/RomFS ──┴──▶ generated shaders (SPIR-V → MSL/DXIL)
                        │
generated/*.c + runtime/ (C++, SDL3) ──clang/MSVC──▶ native yw2 executable
                        │
             at runtime: reads RomFS assets from your dump
```

Players run the build steps on their own dump, so nothing from the game ever ships with the project.
