#!/bin/bash
# PuebloCTR: PuebloPrep (the window PuebloPrep and the console program puebloprep-cli) from the build tree, packed:
#   tools/build_puebloprep.sh linux|windows|macos [VERSION] [OUT_DIR]
# linux: native (x86_64 / arm64); windows: cross-built on Linux with mingw-w64 (tools/mingw-w64-x86_64.cmake);
# macos: universal (arm64 + x86_64). Needs external/CubeEvil4 (tools/fetch_cubeevil4.sh --port-only is enough).
# -> OUT_DIR/PuebloPrep-VERSION-{linux-ARCH.tar.gz,windows-x64.zip,macos-universal.zip}
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OS=$1; VERSION=${2:-dev}; OUT=$(mkdir -p "${3:-$ROOT/build/dist}" && cd "${3:-$ROOT/build/dist}" && pwd)
TREE=${PUEBLO_TREE:-$ROOT/build/tree}
python3 "$ROOT/tools/make_tree.py" "$ROOT/external/CubeEvil4" "$ROOT" "$TREE"
B=$ROOT/build/puebloprep-$OS
ARGS=(-S "$TREE/port/puebloprep" -B "$B" -DCMAKE_BUILD_TYPE=Release -DPUEBLOPREP_WINDOW=ON "-DPUEBLOPREP_VERSION=$VERSION")
case $OS in
  linux)   cmake "${ARGS[@]}" ;;
  windows) cmake "${ARGS[@]}" -DCMAKE_TOOLCHAIN_FILE="$ROOT/tools/mingw-w64-x86_64.cmake" ;;
  macos)   cmake "${ARGS[@]}" -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0 ;;
  *) echo "usage: $0 linux|windows|macos [VERSION] [OUT_DIR]"; exit 2 ;;
esac
cmake --build "$B" --parallel
P=$B/pack/PuebloPrep; rm -rf "$B/pack"; mkdir -p "$P"
mkdir -p "$P/licenses"; cp "$ROOT/LICENSE" "$ROOT/THIRD-PARTY.md" "$P/"
cp "$ROOT/licenses/OFL-1.1.txt" "$P/licenses/"
cp "$B/_deps/sdl3-src/LICENSE.txt" "$P/licenses/SDL3-LICENSE.txt"; cp "$B/_deps/imgui-src/LICENSE.txt" "$P/licenses/imgui-LICENSE.txt"
V=${VERSION// /-}
case $OS in
  linux)
    cp "$B/PuebloPrep" "$B/puebloprep-cli" "$P/"
    N=PuebloPrep-$V-linux-$(uname -m).tar.gz; tar czf "$OUT/$N" -C "$B/pack" PuebloPrep ;;
  windows)
    cp "$B/PuebloPrep.exe" "$B/puebloprep-cli.exe" "$P/"
    N=PuebloPrep-$V-windows-x64.zip; (cd "$B/pack" && rm -f "$OUT/$N" && zip -qr "$OUT/$N" PuebloPrep) ;;
  macos)
    cp -R "$B/PuebloPrep.app" "$P/"; cp "$B/puebloprep-cli" "$P/"
    codesign --force --deep -s - "$P/PuebloPrep.app"; codesign --force -s - "$P/puebloprep-cli"  # (ad hoc: not notarized)
    N=PuebloPrep-$V-macos-universal.zip; (cd "$B/pack" && rm -f "$OUT/$N" && ditto -c -k --norsrc --noextattr --noacl --keepParent PuebloPrep "$OUT/$N") ;;
esac
echo "ok: $OUT/$N"
