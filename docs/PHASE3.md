# Phase 3: graphics (first screens rendered)

The game now draws. On a fresh save it shows "Initializing save data...", then plays the
LEVEL-5 logo movie, then starts the opening movie. The window shows both screens stacked like
the console, and takes keyboard, controller and mouse-as-touch input.

(Update: with audio in place the opening movie plays and the title screen follows; see
[PHASE4.md](PHASE4.md).)

## How rendering works

The 3DS GPU (PICA200) is driven by command lists the game builds in memory and submits through
`gsp::Gpu`. `runtime/src/video/` implements it:

| File | What it does |
|---|---|
| `pica.cpp` | Command-list parser and register file; shader/uniform uploads; vertex arrays, index buffers, immediate mode; primitive assembly (lists, strips, fans); command-buffer jumps |
| `shader.cpp` | Vertex-shader interpreter (full PICA instruction set except geometry-shader EMIT), with the hardware's float quirks (0 × inf = 0, MIN/MAX on NaN) |
| `raster.cpp` | Clipping, triangle setup (4-bit subpixel, consistent fill rule), perspective-correct interpolation, the six-stage TEV combiner, alpha/stencil/depth tests, blending and logic ops, all formats of color/depth buffers |
| `texture.cpp` | All 14 texture formats incl. ETC1/ETC1A4, wrap modes, bilinear filtering |
| `transfer.cpp` | Display transfers (tiled↔linear, format conversion, 2× downscale), TextureCopy, and reading the LCD framebuffers |

The rasterizer is **software** and writes straight into guest VRAM/FCRAM. That makes it correct
by construction for everything that aliases memory: render-to-texture, display transfers into
framebuffers, and CPU reads of rendered images. Phase 2's plan named SDL_GPU for this. A GPU
renderer is still the way to higher resolutions, but it needs a surface cache that tracks
which guest memory lives on the GPU. The software renderer is the reference that path will be
checked against. Draws are rasterized by a thread pool (interleaved 8-row strips), and the menus and movies
run at a full 60 fps.

Vertex shaders are uploaded by the game at run time and executed by an interpreter. Unlike the
CPU code, this is GPU work: there is no ARM code being emulated.

GSP applies framebuffer swaps (`SetBufferSwap` or the shared-memory framebuffer info) at each
VBlank and hands the two LCD images to the frontend.

## Frontend (`runtime/src/frontend/sdl.cpp`)

- SDL3 window, top screen above the bottom one, resizable, F11 for fullscreen. The two images
  are uploaded each frame with SDL_Renderer, on the platform's backend (Metal, Direct3D, Vulkan
  or OpenGL).
- The game runs on its own thread with a 64 MiB host stack. The main thread owns the window,
  as macOS requires.
- Keyboard: arrows = D-pad, WASD = circle pad, K = A, J = B, I = X, U = Y, Q/E = L/R,
  1/3 = ZL/ZR, Enter = Start, Backspace = Select.
- Controllers: by position (Nintendo layout), left stick = circle pad, triggers = ZL/ZR.
- Mouse: left button on the bottom screen = touch.

## Other services this phase needed

- `y2r:u` now converts for real: all input formats, the four output formats, linear or
  8×8-tiled output, the hardware's fixed-point coefficients, and DMA-style gapped buffers. The
  game decodes movies in software and converts each frame here. Rotation is not done yet.
- FS: Get/SetThisSaveDataSecureValue (the anti-rollback value is stored next to the save, not
  in it).
- Recompiler: the RVCT floating-point exception helper takes an inline argument word after
  the `bl`, pops the *caller's* frame, and returns into stubs after that word or to the
  caller's caller. Discovery now recognizes such helpers (`ldr rX, [lr], #4` at entry), ends
  the caller's path at the call, and explores the stubs as separate functions. The emitter
  returns right after the call.

## Debugging aids

| Variable | Effect |
|---|---|
| `YW2_HEADLESS=1` | No window |
| `YW2_DUMP_FRAMES=<dir>[:N]` | Save both screens as PNG every N frames |
| `YW2_DUMP_TEXTURES=<dir>` | Save every distinct texture once (PPM color + PGM alpha) |
| `YW2_PRESS=start@900,a@1000` | Hold a button for 10 frames at a frame number |
| `RECOMP_TRACE=gpu` | Log every draw call (target, textures, first TEV stage, shader entry) |

## Not implemented yet

Fragment lighting, fog, procedural textures, shadow/cube textures, mipmaps, geometry shaders,
and y2r rotation. Also W-buffer polarity: register 0x6D is taken as 1 = z/w, unverified until 3D
scenes render. Guest threads other than the main one still get the platform's default host
stack, which on macOS is 512 KiB.
