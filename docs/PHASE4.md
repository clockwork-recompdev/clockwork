# Phase 4: audio and input (title screen reached)

With audio running, the opening movie plays to the end with its song, the title screen
appears, and the game responds to input: New Game, gender select and name entry all work.
Left idle on the title, it returns to the opening movie as on hardware.

## Audio (`runtime/src/audio/dsp_hle.cpp`)

The 3DS mixes sound on a DSP running Nintendo's firmware. That firmware is replaced by native
code implementing the same shared-memory protocol (layout documented by 3dbrew and Citra):

- Every 160 samples (~4.9 ms at 32728 Hz) it reads the region of DSP RAM the game wrote last
  and writes results to the other one.
- 24 voices: PCM8, PCM16 and DSP-ADPCM buffers, a buffer queue ordered by id, looping and
  embedded buffers, refreshed streaming buffers, linear interpolation at any rate, the simple
  and biquad filters, and per-mix gains with ramps.
- Three intermediate mixes, the aux buses the game can process on the CPU, master volume,
  and mono/stereo output.
- Voice status (position, current/finished buffer ids) goes back to the game. The movie
  player and the music engine clock themselves off it.
- Output goes to an SDL3 audio stream, which resamples to the device rate and keeps latency
  under ~200 ms.

`YW2_DUMP_AUDIO=<file.wav>` records everything played.

## Fixes this phase

- IPC handlers that read their arguments after building the reply got the reply's result
  code instead. One was ConvertProcessAddressFromDspDram, so every DSP structure address
  pointed at 0. Requests now keep a copy of the incoming words, and `arg()` reads that copy.
- `kernel::ticks()` overflowed 64 bits after 68.8 seconds (ns × 268 MHz), which froze every
  timer. Conversions now split off whole seconds.
- Periodic clocks (VBlank, audio frames, HID sampling) run on a fixed schedule. Late ticks
  catch up instead of drifting, so audio stays real-time while the GPU is busy.
- The software rasterizer decodes register state once per draw, skips pass-through TEV stages
  and unused texture units, steps edge functions incrementally, and splits work across a
  thread pool. That took the movies from ~30 fps to a full 60 fps. The recompiled game code
  itself is about 1% of CPU time.

## Debugging aids added

| Variable | Effect |
|---|---|
| `YW2_DUMP_AUDIO=<file.wav>` | Record the audio output |
| `YW2_PROFILE=<file>` | Linux: sample host PCs at 1 kHz, written on SIGTERM/SIGINT; resolve with `addr2line -f -C -e yw2` |
| `RECOMP_TRACE=dsp` | Log voice and mixer configuration changes |

## 3D scenes

Gameplay models use the PICA's per-pixel fragment lighting: the TEV multiplies textures by the
lit color. Without it, every model rendered black and unlit rooms vanished. `video/lighting.cpp`
now implements it:
- up to 8 lights, directional or positional;
- LUT-driven distribution, reflection, Fresnel, spotlight and distance attenuation;
- geometric factors, two-sided diffuse, highlight clamping;
- bump/tangent maps and texture shadows.

Depth fog (128-entry LUT) is in too. The intro cutscenes and the first area (Adams House)
render correctly at 60 fps.

The recompiler's jump-table detection now finds the bounding `cmp` past unrelated scheduled
instructions. That resolved 13 more jump tables, one of which crashed the game during the intro.

`YW2_PRESS` also accepts `touch:X:Y@frame` taps, and `YW2_TRACE_FROM=<frame>` traces three
frames of GPU draws starting at that frame.

## Next

- Procedural textures, cube/shadow textures, mipmaps, gas, as scenes need them.
- y2r rotation, DSP delay/reverb effects, AAC (unused so far).
- am:app is still unimplemented and would stop the game if called.

## GPU renderer

`video/gpu/` moves rasterization and per-pixel work to the GPU through SDL_GPU (Vulkan here;
Metal and Direct3D 12 need the shaders translated, which is still to do). Command lists,
vertex shading and primitive assembly stay on the CPU. The pieces:
- **Uber fragment shader** (`shaders/pica.frag`): the texture units, fragment lighting (LUTs
  in a texture), the TEV combiner in 8-bit integer math like the software renderer, fog and
  the alpha test. Blending, depth, stencil and write masks are cached pipelines.
- **Surfaces:** render targets are GPU textures with rows in memory order, so render-to-texture
  samples them directly. They load from guest memory when the CPU wrote there. Clears and
  display transfers to the LCDs run on the GPU.
- **Upscaling and widescreen:** larger surfaces and a wider frustum, as in the software path.
- **Presentation:** the window's renderer shares the device, so LCD frames are drawn without
  copies.

Gameplay holds the game's 30 fps at 3x widescreen (software renderer: ~2 fps). The software
renderer stays available (launcher: Graphics > GPU renderer) and is what headless runs use.
Not yet on the GPU: logic ops, scissor exclude mode, and transfers from a GPU surface into
non-LCD memory (those would need a readback).
