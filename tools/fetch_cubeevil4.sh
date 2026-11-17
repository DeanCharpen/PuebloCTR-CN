#!/bin/bash
# PuebloCTR: CubeEvil4 into external/CubeEvil4 at the commit in patches/BASE, without the submodule machinery (CI, or a
# copy of the repository downloaded as an archive):
#   tools/fetch_cubeevil4.sh [--port-only]
# --port-only: only port/ (enough for PuebloPrep; macOS and Windows file systems cannot hold CubeEvil4's src/Tools and
# src/tools side by side). A git clone of this repository can use `git submodule update --init` instead.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
URL=$(git config -f "$ROOT/.gitmodules" submodule.external/CubeEvil4.url 2>/dev/null || echo https://github.com/Andiweli/CubeEvil4-Android.git)
BASE=$(cat "$ROOT/patches/BASE")
D=$ROOT/external/CubeEvil4
if [ ! -e "$D/.git" ]; then
  rm -rf "$D"
  if [ "${1:-}" = "--port-only" ]; then
    git clone -q --filter=blob:none --no-checkout --sparse "$URL" "$D"
    git -C "$D" sparse-checkout set port
  else
    git clone -q --filter=blob:none --no-checkout "$URL" "$D"
  fi
fi
git -C "$D" -c advice.detachedHead=false checkout -q "$BASE" 2>/dev/null \
  || { git -C "$D" fetch -q origin "$BASE"; git -C "$D" -c advice.detachedHead=false checkout -q "$BASE"; }
echo "CubeEvil4 $(git -C "$D" rev-parse --short=8 HEAD) in $D"
