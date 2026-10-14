#!/bin/sh
# Builds an optimized Linux release and packages it as yw2-recomp-linux-x86_64.tar.gz.
#
# The package contains the program, the launcher and SDL3, but no game data: whoever runs it
# supplies a dump of their own cartridge (chosen in the launcher). Requires build/generated
# (cargo run --release -p recomp -- emit extracted build/generated), clang and SDL3.
set -e
R=$(cd "$(dirname "$0")/.." && pwd)
O=$R/build/release-obj
OUT=$R/build/release/yw2-recomp
mkdir -p "$O" "$OUT/lib"
JOBS=$(nproc)

# recompiled game code: no call tracing, no FMA contraction, wrapping arithmetic
GAME_FLAGS="-std=c17 -O2 -ffp-contract=off -fwrapv -w -I$R/runtime/include -I$R/build/generated"
ls "$R"/build/generated/*.c | xargs -P "$JOBS" -I{} sh -c \
    'o='"$O"'/$(basename {} .c).o; [ "$o" -nt {} ] || clang '"$GAME_FLAGS"' -c {} -o $o'

SDL=$(pkg-config --cflags sdl3)
IMGUI="-I$R/runtime/third_party/imgui -I$R/runtime/third_party/imgui/backends"
for f in $(find "$R/runtime/src" -name '*.cpp' -o -name '*.c') $(find "$R/runtime/third_party" -name '*.cpp'); do
    case $f in *relay_main.cpp) continue ;; esac # its own program, below
    o=$O/rt_$(echo "${f#$R/runtime/}" | tr / _).o
    case $f in
    *.cpp) C="clang++ -std=c++20 -O2 -w -I$R/runtime/include -I$R/runtime/src $IMGUI $SDL" ;;
    *) C="clang -std=c17 -O2 -I$R/runtime/include" ;;
    esac
    if [ ! "$o" -nt "$f" ] || [ -n "$(find "$R/runtime/src" "$R/runtime/include" -name '*.h' -newer "$o")" ]; then
        $C -c "$f" -o "$o" &
    fi
    while [ "$(jobs -r | wc -l)" -ge "$JOBS" ]; do sleep 0.2; done
done
wait

# the program looks for SDL3 next to itself first (lib/), then on the system
clang++ "$O"/*.o -lm -lpthread $(pkg-config --libs sdl3) -Wl,-rpath,'$ORIGIN/lib' -o "$OUT/yw2"
strip "$OUT/yw2"
cp -L "$(pkg-config --variable=libdir sdl3)/libSDL3.so.0" "$OUT/lib/"
# the relay server for Rooms (just sockets)
clang++ -std=c++20 -O2 -I"$R/runtime/src" "$R/runtime/src/net/relay.cpp" "$R/runtime/src/net/relay_main.cpp" \
    -o "$OUT/clockwork-relay"
strip "$OUT/clockwork-relay"

cat > "$OUT/README.txt" <<'EOF'
Yo-kai Watch 2: Psychic Specters - native PC version (recompiled)
=================================================================

You need your own copy of the game: a decrypted dump of the cartridge (.3ds, .cci or .cxi).
Dump it with GodMode9 on a 3DS: [C:] GAMECART -> "Dump cart (trimmed)", and decrypt it if
needed. The game itself is not included here.

To play
  1. Run ./yw2 (or double-click it).
  2. The launcher opens. On the Game tab, click Browse... and pick your dump.
  3. Adjust Display / Controls / Audio if you like, then press Play.

Default controls
  Arrows: D-Pad    WASD: Circle Pad    K/J/I/U: A/B/X/Y    Q/E: L/R    1/3: ZL/ZR
  Enter: Start     Backspace: Select   Mouse on the bottom screen: touch
  Controllers work too (Nintendo layout: right face button is A). Rebind in the launcher.

In-game keys
  F6 adaptive one-screen layout   F7 widescreen   F8 picture-in-picture   F9 one/two screens
  Tab swap screens   F11 fullscreen   F12 save a capture (screenshot + info) for bug reports

Playtesting: if something looks wrong, press F12 right then; captures go to
~/.local/share/yw2-recomp/captures. If the game stops, the error and a log are worth sending.

Your 3DS save: back it up with Checkpoint or JKSM on the 3DS, copy that backup's folder to the
PC, and use Import save... on the launcher's Library tab.

Saves and settings are kept in ~/.local/share/yw2-recomp. Hold Shift while starting (or run
./yw2 --launcher) to get the launcher back if you turned it off.

Playing together: local wireless (battles, trades, co-op) works between two windows on one PC,
or over the internet through a relay. One player runs ./clockwork-relay on a PC the others can
reach (forward UDP port 47830 to it); everyone enters its address, a room name and their name on
the launcher's Rooms tab, then plays.

Graphics: the GPU renderer needs Vulkan (any reasonably recent graphics driver). Try
Display > Graphics > Internal resolution 2x-4x for a sharper picture, and Widescreen for 16:9.
Without Vulkan it falls back to drawing on the CPU, which is much slower.
EOF

cd "$R/build/release"
tar czf yw2-recomp-linux-x86_64.tar.gz yw2-recomp
echo "packaged $R/build/release/yw2-recomp-linux-x86_64.tar.gz"
