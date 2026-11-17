#!/bin/bash
# The Noto Serif JP faces tools/ui_assets.py and tools/cia_assets.py draw with (SIL Open Font License 1.1:
# licenses/OFL-1.1.txt), from notofonts/noto-cjk at the tag Serif2.003, checked by sha256, into build/fonts.
# Only needed to change the committed pictures (the build does not use them):
#   tools/fetch_fonts.sh
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p build/fonts
cd build/fonts
for f in NotoSerifJP-Regular.otf NotoSerifJP-SemiBold.otf NotoSerifJP-Black.otf; do
  [ -f "$f" ] || curl -fL --retry 3 -o "$f" "https://github.com/notofonts/noto-cjk/raw/Serif2.003/Serif/SubsetOTF/JP/$f"
done
sha256sum -c - <<'SUMS'
2c9a12dbd4f2408c4610c7ee84a108b62d7236c3775baed618c64d9cb44b2f04  NotoSerifJP-Regular.otf
116d06c2b11ceba33ccb3f8c9eb1c86aba3d5761a1199fd37f74e83365e7a53d  NotoSerifJP-SemiBold.otf
b7197366b775ccb6cd3473b7b09f2c5759a2fdfdbfedf975029203828d0ad386  NotoSerifJP-Black.otf
SUMS
