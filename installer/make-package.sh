#!/bin/bash
# Builds the Endless Keeper package from this checkout: the game, its DLLs and game data, made the way KeeperFX
# makes its alpha patches, plus the Unicode fonts and the Endless Mode stub level's map files. The setup
# (installer/endless-keeper.iss) carries this package and lays it over the KeeperFX base game.
#
# Usage (Linux or WSL, with the tools of the Development Guide):  installer/make-package.sh [output.7z]
# Output: pkg/endless-keeper.7z, or the given file.
set -euo pipefail
cd "$(dirname "$0")/.."
OUTPUT=${1:-pkg/endless-keeper.7z}
# The KeeperFX build Endless Keeper is built on (the version the game reports; it must match the game data).
BUILD_NUMBER=${BUILD_NUMBER:-$(cat installer/keeperfx-build.txt)}
SUFFIX=Alpha
# Endless Keeper's own release number and stage, on the main menu over the KeeperFX version (the release workflow
# sets them; any other build says "dev")
ENDLESS_RELEASE=${ENDLESS_RELEASE:-dev}
ENDLESS_STAGE=${ENDLESS_STAGE:-Alpha}
VER=(BUILD_NUMBER=$BUILD_NUMBER PACKAGE_SUFFIX=$SUFFIX ENDLESS_RELEASE=$ENDLESS_RELEASE ENDLESS_STAGE=$ENDLESS_STAGE)
echo "KeeperFX build $BUILD_NUMBER, Endless Keeper $ENDLESS_RELEASE - $ENDLESS_STAGE"

echo "== engine graphics"
make "${VER[@]}" pkg-enginegfx > /dev/null
echo "== game"
cmake -S . -B out -G Ninja -DCMAKE_TOOLCHAIN_FILE=build/cmake/toolchains/mingw32.cmake \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBUILD_NUMBER=$BUILD_NUMBER -DPACKAGE_SUFFIX=$SUFFIX \
  -DENDLESS_RELEASE=$ENDLESS_RELEASE -DENDLESS_STAGE=$ENDLESS_STAGE > /dev/null
cmake --build out --target keeperfx keeperfx_hvlog > /dev/null
echo "== game, 64-bit"
cmake -S . -B out64 -G Ninja -DCMAKE_TOOLCHAIN_FILE=build/cmake/toolchains/mingw64.cmake \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBUILD_NUMBER=$BUILD_NUMBER -DPACKAGE_SUFFIX=$SUFFIX \
  -DENDLESS_RELEASE=$ENDLESS_RELEASE -DENDLESS_STAGE=$ENDLESS_STAGE > /dev/null
cmake --build out64 --target keeperfx > /dev/null
echo "== game data"
make "${VER[@]}" pkg-assemble > /dev/null

echo "== 64-bit game in x64 (with its SDL3 DLLs; started there, it plays from the game folder above)"
mkdir -p pkg/x64
cp out64/keeperfx.exe pkg/x64/
cp out64/deps/sdl3/SDL3-*/x86_64-w64-mingw32/bin/SDL3.dll \
   out64/deps/sdl3_mixer/SDL3_mixer-*/x86_64-w64-mingw32/bin/SDL3_mixer.dll \
   out64/deps/sdl3_image/SDL3_image-*/x86_64-w64-mingw32/bin/SDL3_image.dll pkg/x64/

echo "== Unicode fonts"
( cd tools/fxfontmaker
  python3 rescale_unifont_hex.py unifont-17.0.04.hex unifont12.hex > /dev/null
  python3 bdf_to_hex.py wenquanyi_9pt.bdf wenquanyi.hex > /dev/null
  python3 merge_hex.py unifont12.hex wenquanyi.hex merged12.hex > /dev/null
  python3 unifont_hex_to_binary.py unifont-17.0.04.hex font16.fxfont 16 > /dev/null
  python3 unifont_hex_to_binary.py unifont_jp-17.0.04.hex font16_jpn.fxfont 16 > /dev/null
  python3 unifont_hex_to_binary.py unifont_t-17.0.04.hex font16_cht.fxfont 16 > /dev/null
  python3 unifont_hex_to_binary.py merged12.hex font12.fxfont 12 > /dev/null
  rm -f merged12.hex wenquanyi.hex unifont12.hex
  mkdir -p ../../pkg/fxdata && mv font*.fxfont ../../pkg/fxdata/ )

echo "== Endless Mode stub level"
mkdir -p pkg/levels/endless
find levels/endless -maxdepth 1 -type f ! -name '*.txt' -exec cp {} pkg/levels/endless/ \;

echo "== package"
rm -f pkg/keeperfx-*.7z
cmake --build out --target package > /dev/null
mkdir -p "$(dirname "$OUTPUT")"
mv pkg/keeperfx-*.7z "$OUTPUT"
ls -la "$OUTPUT"
