#!/usr/bin/env python3
"""PuebloCTR: the build tree - CubeEvil4 at the commit in patches/BASE, PuebloCTR's own files over it, PuebloCTR's
changes of CubeEvil4's files applied, the CubeEvil4 files PuebloCTR does not use removed.

    python3 tools/make_tree.py CUBEEVIL4_CHECKOUT PUEBLOCTR_ROOT OUT_TREE

CUBEEVIL4_CHECKOUT: the submodule (external/CubeEvil4) at patches/BASE. OUT_TREE is made anew (a copy; the submodule
stays untouched).
"""
import os
import shutil
import subprocess
import sys
from pathlib import Path


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    sub, root, out = (Path(a).resolve() for a in sys.argv[1:])
    base = (root / "patches/BASE").read_text().strip()
    # (safe.directory: a checkout owned by another user - a bind mount into the build image - is fine here)
    head = subprocess.run(["git", "-c", "safe.directory=*", "-C", str(sub), "rev-parse", "HEAD"], capture_output=True,
                          text=True).stdout.strip()
    if head != base:
        sys.exit(f"make_tree: CubeEvil4 is at {head or '?'}, the patches are for {base} (git submodule update --init)")
    if out.exists():
        shutil.rmtree(out)
    shutil.copytree(sub, out, symlinks=True, ignore=shutil.ignore_patterns(".git"))
    overlay = root / "overlay"
    n = 0
    for f in overlay.rglob("*"):
        if f.is_file():
            dst = out / f.relative_to(overlay)
            dst.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(f, dst)
            n += 1
    patch = root / "patches/cubeevil4.patch"
    if patch.stat().st_size:
        # (git looks for a repository above OUT_TREE - this one, when build/ is inside it - and would then skip every
        # path of the patch without a word: the search stops at OUT_TREE, and the applied files are counted)
        env = dict(os.environ, GIT_CEILING_DIRECTORIES=str(out.parent), LC_ALL="C")  # (C: git's messages counted)
        r = subprocess.run(["git", "apply", "-v", "--unidiff-zero", "--whitespace=nowarn", str(patch)], cwd=out, env=env,
                           capture_output=True, text=True)
        files = patch.read_bytes().count(b"\ndiff --git ") + patch.read_bytes().startswith(b"diff --git ")
        applied = r.stderr.count("Applied patch ")
        if r.returncode or applied != files:
            sys.exit(f"make_tree: the patch: {applied} of {files} files applied\n{r.stderr[-2000:]}")
    removed = [l.strip() for l in (root / "patches/cubeevil4-removed.txt").read_text().splitlines() if l.strip()]
    for p in removed:
        (out / p).unlink(missing_ok=True)
    print(f"make_tree: {out}: CubeEvil4 {base[:8]}, {n} files of PuebloCTR, patch applied, {len(removed)} removed")


if __name__ == "__main__":
    main()
