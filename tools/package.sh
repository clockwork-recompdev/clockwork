#!/usr/bin/env bash
# Packages a release of Clockwork for this OS: dist/clockwork-<name>.zip (Windows) or .tar.gz.
#
#   tools/package.sh <name> <folder with the built clockwork and clockwork-relay>
#
# The package holds the clockwork program (launcher and runtime, no game), the libraries it needs,
# the tools that unpack and recompile a dump (tools/), and the sources compiled with a game's
# generated code into its module (sdk/). Whoever runs it supplies dumps of their own cartridges;
# the launcher builds each game from them on their PC. Expects `cargo build --release` done.
set -euo pipefail
R=$(cd "$(dirname "$0")/.." && pwd)
NAME=$1
BIN=$2
OUT=$R/dist/clockwork-$NAME
rm -rf "$OUT"
mkdir -p "$OUT/tools" "$OUT/sdk"

case "$(uname -s)" in
MINGW* | MSYS* | CYGWIN*) OS=windows X=.exe ;;
Darwin) OS=macos X= ;;
*) OS=linux X= ;;
esac

cp "$BIN/clockwork$X" "$BIN/clockwork-relay$X" "$OUT/"
cp "$R/target/release/ctrx$X" "$R/target/release/recomp$X" "$OUT/tools/"
cp "$R/runtime/include/recomp.h" "$R/runtime/include/recomp_module.h" "$R/runtime/module/module.c" \
    "$R/runtime/src/arm_ops.c" "$OUT/sdk/"

case $OS in
windows)
    cp "$BIN"/*.dll "$OUT/" # SDL3, curl, libarchive and theirs, put there by vcpkg
    ;;
macos)
    # Homebrew's libraries, copied in and pointed at from the program
    dylibbundler -od -b -x "$OUT/clockwork" -d "$OUT/lib" -p @executable_path/lib/ > /dev/null
    # changing their install names undid their signatures; Apple Silicon runs only signed code
    for f in "$OUT/clockwork" "$OUT/lib"/*.dylib; do codesign --force -s - "$f"; done
    ;;
linux)
    # SDL3 goes with it (few distributions have it yet); curl and libarchive are everywhere
    mkdir -p "$OUT/lib"
    sdl=$(ldd "$OUT/clockwork" | awk '/libSDL3/ {print $3}')
    cp -L "$sdl" "$OUT/lib/"
    patchelf --set-rpath '$ORIGIN/lib' "$OUT/clockwork"
    strip "$OUT/clockwork" "$OUT/clockwork-relay"
    ;;
esac

case $OS in
windows) COMPILER='Visual Studio'"'"'s C++ compiler: install "Build Tools for Visual Studio" (free, from
  visualstudio.microsoft.com/downloads) with "Desktop development with C++".' ;;
macos) COMPILER='Apple'"'"'s C compiler: run  xcode-select --install  in Terminal.' ;;
linux) COMPILER='a C compiler: gcc or clang (the build-essential or base-devel package).' ;;
esac
case $OS in
windows) DATA='%APPDATA%\yw2-recomp' RUN='clockwork.exe' ;;
macos) DATA='~/Library/Application Support/yw2-recomp' RUN='./clockwork from Terminal. The first time, let macOS run
     programs that aren'"'"'t from the App Store:  xattr -dr com.apple.quarantine <this folder>' ;;
linux) DATA='~/.local/share/yw2-recomp' RUN='./clockwork' ;;
esac

cat > "$OUT/README.txt" <<EOF
Clockwork: the Yo-kai Watch 3DS games, recompiled for PC
========================================================

No game is included. You play from decrypted dumps of cartridges you own: dump each one with
GodMode9 on a 3DS ([C:] GAMECART -> "Dump cart (trimmed)", then "Decrypt file" if needed).

Clockwork builds each game from its dump on your PC, once. That needs
$COMPILER

To play
  1. Run $RUN
  2. The launcher opens. Click Browse... and pick a dump; dumps next to it are found too.
  3. The game builds (a few minutes, shown on the Library tab), then press Play.

Default controls
  Arrows: D-Pad    WASD: Circle Pad    K/J/I/U: A/B/X/Y    Q/E: L/R    1/3: ZL/ZR
  Enter: Start     Backspace: Select   Mouse on the bottom screen: touch
  Controllers work too (Nintendo layout: right face button is A). Rebind in the launcher.

In-game keys
  F5 layout for this scene   F6 adaptive layout   F7 widescreen   F8 picture-in-picture
  F9 one/two screens   Tab swap screens   F11 fullscreen   F12 capture (for bug reports)

Saves, settings, mods and the built games are kept in $DATA
Import a save from a 3DS (a Checkpoint or JKSM backup) on the Library tab.

Playing together (Rooms): local wireless (battles, trades, co-op) between two windows on one PC,
or over the internet through a relay. Someone runs clockwork-relay where the others can reach it
(UDP port 47830); everyone picks it, a room and a name on the Rooms tab.

tools/ holds ctrx (unpacks a dump) and recomp (recompiles it to C), which the launcher runs;
sdk/ holds the sources compiled with a game's code. Source: github.com/clockwork-recompdev/clockwork
EOF

cd "$R/dist"
if [ $OS = windows ]; then
    7z a "clockwork-$NAME.zip" "clockwork-$NAME" > /dev/null
    echo "packaged dist/clockwork-$NAME.zip"
else
    tar czf "clockwork-$NAME.tar.gz" "clockwork-$NAME"
    echo "packaged dist/clockwork-$NAME.tar.gz"
fi
