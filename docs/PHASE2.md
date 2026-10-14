# Phase 2: runtime (boot milestone reached)

`build/yw2 <dump.3ds>` runs the recompiled game natively. It boots through all of the game's
startup code and system initialization and settles into a steady main loop: about 417 GX
command submissions per second (~7 per frame at 60 fps) with no hangs. Nothing is drawn yet;
rendering is Phase 3.

## Runtime layout (`runtime/src`)

| File | What it does |
|---|---|
| `main.cpp` | Startup, guest fault reporting (with call trace), Ctrl+\ thread dump |
| `dump.cpp` | Reads the player's dump directly: exheader, BLZ-compressed `.code`, RomFS level 3 |
| `memory.cpp` | 4 GiB reserved guest address space; pages committed only where the 3DS maps memory |
| `loader.cpp` | Maps code/data, clears `.bss` |
| `kernel/` | HLE kernel: threads, scheduler, waits, timers, address arbiters, ~45 SVCs |
| `hle/` | Services (below) and the IPC framework |
| `platform.cpp` | Per-user data folder |

## Kernel

- Each guest thread is a host thread; only the scheduler's current thread runs guest code.
  Switches happen at SVCs: blocking hands the CPU to the highest-priority ready thread, and a
  thread woken by a higher-priority event takes over at the running thread's next SVC.
- Events (one-shot/sticky/pulse), mutexes, semaphores, timers, address arbiters (all wait
  types), WaitSynchronization1/N with timeouts, shared memory (service blocks are placed where
  the game maps them), ControlMemory heap/linear allocation, QueryMemory.

## Services

| Service | State |
|---|---|
| srv: | Service sessions, notifications |
| APT | Startup handshake (lock, events, wakeup parameter), applet queries, CPU time limit |
| fs:USER | RomFS from the dump; save data, Play Coins shared extdata (`gamecoin.dat`) in the user data folder; files, directories, format |
| cfg | US-region English 3DS defaults for the config blocks the game reads |
| hid | Shared-memory input rings updated at 60 Hz (no buttons yet) |
| ptm | Charging, lid open, full battery, no steps |
| gsp::Gpu | Rights, register file, interrupt queues, VBlank, GX queue (fills and DMA done; command lists and display transfers accepted, not rendered) |
| dsp::DSP | Startup protocol: component load, audio pipe init reply, frame clock (no mixing) |
| y2r | Configuration accepted; conversions complete instantly (no output) |
| cecd | StreetPass idle; mailbox files in the user data folder |
| ndm, boss, nim:aoc | Offline stubs |
| err:f | Prints the game's fatal error reports |

## Debugging aids

- `RECOMP_TRACE=svc,ipc` logs system calls and service commands.
- Builds with `-DRECOMP_TRACE_CALLS` keep the last 64 guest calls per thread and print them on
  any fatal error or guest memory fault.
- `kill -QUIT` (Ctrl+\) prints every guest thread, its last SVC and what it waits on.
- `recomp fn extracted <addr>` explains whether an address is a function and why not.

## Recompiler changes found while booting

Discovery now also handles: resolving register calls after a function is fully explored,
direct call targets whose code is shared with another function, vtable-like pointer runs,
pointers onto shared instruction boundaries, PC-relative register adds (`add rX, pc`), Thumb
gap filling, rejection sets keyed by mode, and three hand-written-assembly patterns in the video
decoder: relative jump tables, calls made with a hand-set LR (both `bx rN` and plain `b`), and
returns through a register holding the saved LR. CP15 barriers compile to nothing.
