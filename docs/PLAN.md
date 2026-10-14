# Plan

Design rules (native code only, no interpreter/JIT, Windows/macOS/Linux) are in
[ARCHITECTURE.md](ARCHITECTURE.md).

| Phase | Work | Milestone |
|---|---|---|
| 0. Analysis | Unpack the dump; map code segments; find CRO modules and which system services are used | Known scope of code to translate |
| 1. Recompiler | ARM/Thumb (ARMv6K + VFPv2) → C; function discovery; jump-table and indirect-call resolution | All game code compiles natively |
| 2. Runtime | Memory map, SVCs, threads/sync, HLE services (fs, hid, apt, gsp, ...), RomFS read from the dump | Boots; game logic reaches its first frame |
| 3. Graphics | PICA200 → SDL_GPU (Vulkan/Metal/D3D12); shaders translated at build time; dual-screen layout | Title screen renders on all three OSes |
| 4. Audio & input | Native audio (no DSP emulation); touch → mouse; controller support | Playable |
| 5. Enhancements | Resolution scaling, widescreen, framerate, mod loading | |
| 5b. Rooms | Local wireless play (nwm::UDS: battles, trades, co-op) over the internet, through rooms on a relay server. See below | Two players in one room battle as if side by side |
| 6. Nintendo Switch | Clockwork itself running on a (homebrew) Switch: the same recompiled games, built for AArch64 with devkitPro/libnx. See below | A game playable in handheld mode, the touch screen as the 3DS bottom screen |

## Phase 5b: Rooms (local wireless over the internet)

Every game here uses local wireless (`nwm::UDS`) for battles, trades and co-op. The game hands the
system raw frames for "nearby" consoles; Clockwork carries them to the other players in a room,
so the game believes they're nearby. (Citra and Azahar do the same with their multiplayer rooms;
their code is GPL, so we follow the behavior, not the code.) Nintendo Network online modes are
out of scope: those servers are gone and reviving them is per-game server work.

1. **Learn the calls**: `runtime/src/hle/uds.cpp` answers safely (nobody nearby) and logs every
   command; each game's local-play menus are traced to see what it uses. Before this, opening local
   play stopped the game.
2. **Two windows on one PC**: hosting, scanning (beacons), joining, nodes, bind/send/pull with
   their events, between two Clockwork processes through a local relay.
3. **Relay server and Rooms tab**: a small standalone server in the repo that relays frames among a
   room's members; the launcher's Rooms tab creates/joins rooms and shows who's there. Done:
   `clockwork-relay` (`runtime/src/net/relay.cpp`, the same code as the hub for two windows) with
   named rooms, optional passwords, a token handshake so nobody can sign up someone else's address,
   and answers never larger than the request; the game joins its room when it starts.
4. **Over the internet**: the same with the relay hosted anywhere. Everyone needs the same game
   (versions that play together on hardware, like Bony Spirits and Fleshy Souls, do here too).

Open questions: which commands each game uses (step 1); whether beacon data encryption matters
when both ends are Clockwork (likely not: we produce and decrypt our own); latency tolerance of
each game's battle protocol over the internet.

## Phase 6: Clockwork on Nintendo Switch

The recompiled games are portable C, so they should build for the Switch's AArch64 CPU. The
work is everything around them, where the PC design rules don't hold. A homebrew target: no
official SDK, nothing signed, players run it from the Homebrew Menu with their own dumps on the
SD card, as on PC.

| Area | PC today | On Switch |
|---|---|---|
| Toolchain | clang/MSVC, CMake | devkitA64 (GCC) + libnx; a CMake toolchain file. Each game is an `.nro` |
| Guest memory | One 4 GiB reserved block, `base + guest_addr` | libnx can reserve virtual address space; map only the regions the game uses (code, heap, VRAM, shared memory) on demand. Check first: this decides whether the generated code works unchanged |
| Graphics | SDL_GPU only (Vulkan/Metal/D3D12), SPIR-V shaders | SDL3 has no public Switch backend. A second backend on deko3d (homebrew's low-level GPU API), or OpenGL via Mesa. Shaders built for it at build time. The biggest piece |
| Window, input, audio | SDL3 | libnx directly (or devkitPro's SDL port): Joy-Con/Pro Controller, the touch screen as the bottom screen in handheld mode, `audren` for sound |
| Launcher and handoff | ImGui launcher; `execv` to the game's executable | The same ImGui UI on the new backend; hand off with `envSetNextLoad` (chain-loading the game's `.nro`) |
| Saves and data | `~/.local/share/yw2-recomp` | `sdmc:/switch/clockwork/`, the same per-game layout |
| Dumps | Any file size | FAT32 SD cards cap files at 4 GiB, and untrimmed dumps hit it (Psychic Specters' is exactly 4 GiB). Support trimmed dumps, split files, or require exFAT |

Milestones, in order:

1. **Builds**: runtime and one game's generated code compile and link as an `.nro`.
2. **Boots**: guest memory mapped; the game reaches its first command list headless, logging over nxlink.
3. **Draws**: the second GPU backend renders the title screen.
4. **Plays**: input, touch, audio and the launcher; handheld and docked layouts.
5. **Runs at speed**: the Switch's CPU (4x Cortex-A57 at about 1 GHz, 3 cores for games) is several
   times slower than a PC. Today YW2 spends up to about 160 ms per second on command processing and vertex
   shading, spread over host cores; on Switch that could approach the whole frame budget at 30 fps.
   Profile on hardware early (step 2), before committing to the rest.

Open questions for Phase 6:

- Can libnx reserve and sparsely map a 4 GiB region for an application (not an applet)? If not, the
  generated code needs a different memory scheme on Switch.
- deko3d or OpenGL? deko3d is faster but means a second renderer from scratch; OpenGL via Mesa
  is closer to existing renderers but slower on this hardware.
- Does the frame budget hold on the A57 for YW2 and Blasters, and how much of the vertex shading
  should move to the GPU?

## Open questions (answer in Phase 0)

- Does the game load CROs (dynamic modules)? Each needs relocating and recompiling too.
- Which services does it call? (Scan `svc 0x32` / `SendSyncRequest` call sites and service name strings in `.rodata`.)
- Any self-modifying code or code generated at runtime?
- How large is `.text`, and how much is ARM vs Thumb?

## References

- Azahar (Citra successor): HLE services and PICA200 behavior. GPL; copying code makes this project GPL.
- 3dbrew.org: file formats, SVCs, services.
- N64Recomp / XenonRecomp: structure of existing static recompilers.
