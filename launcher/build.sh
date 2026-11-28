#!/bin/bash
# Builds ek-launcher.exe (the launcher) with the engine's i686 mingw compiler: one 32-bit program for every Windows.
# Needs the engine build's SDL3 (out/deps/sdl3) and Dear ImGui 1.92.4 (fetched into out/deps once), and src/ver_defs.h
# (made when the engine is configured: the launcher shows and compares that version).
# Usage: launcher/build.sh [output folder]   (default: out/launcher)
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
IMGUI="${IMGUI_DIR:-$ROOT/out/deps/imgui-1.92.4}"
if [ ! -f "$IMGUI/imgui.h" ]; then
    echo "Fetching Dear ImGui 1.92.4"
    mkdir -p "$IMGUI"
    curl -fsSL --retry 5 https://github.com/ocornut/imgui/archive/refs/tags/v1.92.4.tar.gz | tar -xz -C "$IMGUI" --strip-components=1
fi
SDL="$(ls -d "$ROOT"/out/deps/sdl3/SDL3-*/i686-w64-mingw32 | head -1)"
OUT="${1:-$ROOT/out/launcher}"
mkdir -p "$OUT"
OUT="$(cd "$OUT" && pwd)" # (a full path: the resources are compiled from the launcher's folder)
OBJ="$OUT/obj"
mkdir -p "$OBJ"
CXX=i686-w64-mingw32-g++
CXXFLAGS="-std=c++17 -O2 -DNDEBUG -msse2 -I$IMGUI -I$IMGUI/backends -I$SDL/include"
objs=()
compile() { # source object [warnings]
    if [ ! -f "$2" ] || [ "$1" -nt "$2" ] || [ -n "$(find "$HERE" "$ROOT/src/ver_defs.h" -name '*.h' -newer "$2" 2>/dev/null | head -1)" -a -n "$3" ]; then
        echo "  $(basename "$1")"
        $CXX $CXXFLAGS $3 -c "$1" -o "$2"
    fi
    objs+=("$2")
}
for f in imgui imgui_draw imgui_tables imgui_widgets; do
    compile "$IMGUI/$f.cpp" "$OBJ/$f.o"
done
for f in imgui_impl_sdl3 imgui_impl_sdlrenderer3; do
    compile "$IMGUI/backends/$f.cpp" "$OBJ/$f.o"
done
WARN="-Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers"
compile "$HERE/launcher.cpp" "$OBJ/launcher.o" "$WARN -Wno-unused-function"
compile "$HERE/engine_view.cpp" "$OBJ/engine_view.o" "$WARN"
compile "$HERE/fx.cpp" "$OBJ/fx.o" "$WARN"
compile "$HERE/heart_sound.cpp" "$OBJ/heart_sound.o" "$WARN"
compile "$HERE/net.cpp" "$OBJ/net.o" "$WARN"
compile "$HERE/migrate.cpp" "$OBJ/migrate.o" "$WARN"
(cd "$HERE" && i686-w64-mingw32-windres -c 65001 launcher.rc -O coff -o "$OBJ/launcher_res.o")
# the launcher is ek-launcher.exe since 2026-11-28 (it was EndlessKeeper.exe, which is now the game's name)
$CXX -o "$OUT/ek-launcher.exe" "${objs[@]}" "$OBJ/launcher_res.o" "$SDL/lib/libSDL3.dll.a"     -mwindows -static -lwinhttp -lbcrypt -lshell32 -ladvapi32 -luser32 -lgdi32 -limm32 -lole32 -loleaut32 -lversion -lsetupapi -luuid
i686-w64-mingw32-strip "$OUT/ek-launcher.exe"
echo "Built $OUT/ek-launcher.exe ($(stat -c %s "$OUT/ek-launcher.exe") bytes)"
# the bridge keeperfx.exe for launchers of 18.1 and older (keeperfx_bridge.c): packed as keeperfx.exe and x64\keeperfx.exe
i686-w64-mingw32-gcc -std=c11 -O2 -municode -mwindows -static -Wall -Wextra "$HERE/keeperfx_bridge.c" -o "$OUT/keeperfx-bridge.exe"
i686-w64-mingw32-strip "$OUT/keeperfx-bridge.exe"
echo "Built $OUT/keeperfx-bridge.exe ($(stat -c %s "$OUT/keeperfx-bridge.exe") bytes)"
