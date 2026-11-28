#!/bin/bash
# Builds Endless Keeper's native Linux (x86_64) archive from this checkout: the keeperfx program with the libraries it
# needs that are not part of every Linux system in lib/ (KFX_DECK_BUNDLE), and the same game data as the Windows
# package (installer/make-package.sh: KeeperFX's data pipeline, the Unicode fonts, the Endless Mode stub level).
# A .tar.gz keeps the program's executable bit. It goes over a KeeperFX 1.4.0 install with the original Dungeon
# Keeper files; the result runs on Linux with glibc 2.39 or newer (Ubuntu 24.04 and later), so build it on Ubuntu 24.04.
#
# Usage (Ubuntu 24.04 with the packages of .github/actions/build-linux):  installer/make-linux-package.sh [output.tar.gz]
# Output: pkg/endless-keeper-linux_x86_64.tar.gz, or the given file.
set -euo pipefail
cd "$(dirname "$0")/.."
OUTPUT=${1:-pkg/endless-keeper-linux_x86_64.tar.gz}
# The KeeperFX build Endless Keeper is built on (the version the game reports; it must match the game data).
BUILD_NUMBER=${BUILD_NUMBER:-$(cat installer/keeperfx-build.txt)}
SUFFIX=Alpha
ENDLESS_RELEASE=${ENDLESS_RELEASE:-dev}
ENDLESS_STAGE=${ENDLESS_STAGE:-Alpha}
JOBS=${JOBS:-$(nproc)}
VER=(BUILD_NUMBER=$BUILD_NUMBER PACKAGE_SUFFIX=$SUFFIX ENDLESS_RELEASE=$ENDLESS_RELEASE ENDLESS_STAGE=$ENDLESS_STAGE)
echo "KeeperFX build $BUILD_NUMBER, Endless Keeper $ENDLESS_RELEASE - $ENDLESS_STAGE, Linux $(uname -m)"

echo "== engine graphics"
make "${VER[@]}" pkg-enginegfx > /dev/null
echo "== game"
# configure fetches SDL3 and a few prebuilt libraries from GitHub, which sometimes fails for a moment: once more
configure() {
  cmake -S . -B out-linux -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DKFX_DECK_BUNDLE=ON -DKFX_SPLIT_DEBUG=ON \
    -DBUILD_NUMBER=$BUILD_NUMBER -DPACKAGE_SUFFIX=$SUFFIX \
    -DENDLESS_RELEASE=$ENDLESS_RELEASE -DENDLESS_STAGE=$ENDLESS_STAGE > /dev/null
}
configure || { sleep 30; configure; }
cmake --build out-linux --target keeperfx keeperfx_hvlog -j"$JOBS" > out-linux/build.log 2>&1 \
  || { grep -E "error|FAILED" out-linux/build.log | head -30; tail -5 out-linux/build.log; exit 1; }
echo "== game data"
make "${VER[@]}" pkg-assemble > /dev/null

# the same Endless Keeper additions as installer/make-package.sh (keep the two in step)
echo "== the Zombie's eye palette (its mod is in mods/zombie, switched on by mods/load_order.cfg)"
mkdir -p pkg/data
cp config/mods/zombie/data/zompal.pal pkg/data/
rm -rf pkg/mods/zombie/data

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

echo "== the modern UI's improved icons (config/fxdata/ui_icons -> fxdata/ui_icons)"
mkdir -p pkg/fxdata/ui_icons
cp -r config/fxdata/ui_icons/. pkg/fxdata/ui_icons/

echo "== Endless Mode stub level"
mkdir -p pkg/levels/endless
find levels/endless -maxdepth 1 -type f ! -name '*.txt' -exec cp {} pkg/levels/endless/ \;

echo "== package (the program, lib/ and the game data)"
rm -f pkg/keeperfx-linux_*.tar.gz
cmake --build out-linux --target package > out-linux/package.log || { tail -30 out-linux/package.log; exit 1; }
mkdir -p "$(dirname "$OUTPUT")"
mv pkg/keeperfx-linux_*-patch.tar.gz "$OUTPUT"

echo "== check: every library is found, none from the build folder"
check=$(mktemp -d)
tar -xzf "$OUTPUT" -C "$check"
ldd "$check/endlesskeeper" > "$check/ldd.txt" # (the game's name in the packages since 2026-11-28: Packaging.cmake)
if grep -E "not found|$PWD" "$check/ldd.txt"; then
  echo "endlesskeeper does not find all its libraries in the archive"
  exit 1
fi
echo "$(ls "$check/lib" | wc -l) files in lib/, $(grep -c "$check/lib" "$check/ldd.txt") libraries loaded from there"
rm -rf "$check"
ls -la "$OUTPUT"
