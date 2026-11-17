#!/bin/bash
# PuebloCTR: everything from the repository, inside the build image (docker/Dockerfile):
#   tools/build.sh [--release] [--version TEXT] [--jobs N]
# 1. the build tree: CubeEvil4 (external/CubeEvil4, the submodule) + PuebloCTR's files and patches (tools/make_tree.py)
# 2. the tree's Linux build up to the link plan (prepared sources, stubs, module imports, the compile commands)
# 3. the game for the 3DS (tools/build3ds.py) -> build/tree/port/build/3ds-release/PuebloCTR.3dsx (or 3ds-dev/)
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
TREE=${PUEBLO_TREE:-$ROOT/build/tree}
python3 "$ROOT/tools/make_tree.py" "$ROOT/external/CubeEvil4" "$ROOT" "$TREE"
python3 "$ROOT/tools/minimap_gen.py" "$TREE"  # (the minimap's copy of the map screen's rules: the game's code)
L=$TREE/port/build/whole-armhf-O2-low-static
cmake -S "$TREE/port/whole" -B "$L" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$TREE/port/whole/cmake/linux-armhf.cmake" \
      -DRE4_MEM1_BASE=0x08000000 -DRE4_OPT_LEVEL=2 -DRE4_STATIC_MODULES=ON >/dev/null
ninja -C "$L" re4_link_plan
python3 "$ROOT/tools/build3ds.py" --tree "$TREE" "$@"
