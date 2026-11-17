#!/bin/bash
# PuebloCTR.cia from the release build (tools/build.sh --release), inside the build image (makerom, bannertool):
#   tools/build_cia.sh [VERSION]       -> build/tree/port/build/3ds-release/PuebloCTR.cia
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
TREE=${PUEBLO_TREE:-$ROOT/build/tree}
C=$TREE/port/whole/3ds/cia; O=$TREE/port/build/3ds-release
bannertool makebanner -i "$C/banner.png" -a "$C/banner.wav" -o "$O/banner.bnr"
bannertool makesmdh -s "PuebloCTR" -l "Unofficial New 3DS port, ${1:-dev}" -p "PuebloCTR" -i "$C/icon.png" -o "$O/icon.smdh"
makerom -f cia -o "$O/PuebloCTR.cia" -elf "$O/re4.elf" -rsf "$C/PuebloCTR.rsf" -icon "$O/icon.smdh" -banner "$O/banner.bnr" -target t
ls -la "$O/PuebloCTR.cia"
