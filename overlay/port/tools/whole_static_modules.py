#!/usr/bin/env python3
"""Static REL modules (RE4_STATIC_MODULES, milestone 3DS-4.3).

The 3DS has no dynamic loader: every REL module goes into the one program image.
The shared-library build (one libre4_<mod>.so per module, dlopen RTLD_LOCAL,
-Bsymbolic) gives each module properties the static image has to keep:

  - its own copies of the units the original RELs duplicated (em_wrap.cpp,
    st2.cpp, ...) and of inline functions and their static locals;
  - a lookup of a symbol in one module (dlsym): _prolog, the cross-module
    imports (re4_try_resolve_import), re4_module_reset_imports_<mod>, ...;
  - fresh data on every load: OSLink unloads a module to bring back its initial
    data, as reading the REL from disc did on the GameCube;
  - static constructors when it loads, destructors (__cxa_finalize) per module.

Per module this tool makes one relocatable object out of the module's objects
(`ld -r`) and with objcopy:

  - drops the COMDAT groups (.group): the final link would keep one copy of a
    group across modules even with local signature symbols;
  - makes every defined symbol local except the ones looked up by name, which
    are renamed re4s_<mod>__<name>; `new`/`delete` go to the game heap
    (--wrap, as the module libraries are linked);
  - renames .data*/.data.rel.ro* to re4d_<mod>, .bss* to re4b_<mod> and
    .init_array* to re4i_<mod>, so that __start_/__stop_ symbols bound them.

static_modules.cpp registers the table (name, exported symbols, constructors,
data, bss) with the module loader (runtime/module_loader.cpp). With --ldscript a linker script
fragment places those sections inside the data segment of devkitARM's 3dsx.ld, where the program
image ends at __end__ (orphan sections would go after it): data and constructors after .dtors,
bss right after .bss (cleared with it, before __bss_end__).
"""
import argparse
import re
import subprocess
import sys
from pathlib import Path

# looked up by name in every module (module_loader.cpp, os_link.cpp, host/main.cpp)
SPECIAL = {"_prolog", "_epilog", "_unresolved", "re4_module_finalize"}
SPECIAL_PREFIX = ("re4_module_reset_imports_", "re4_module_check_imports_")
IMPORT_NAME = re.compile(r'^    "(.*)",$')


def run(cmd: list) -> str:
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit(f"{' '.join(cmd[:4])} ...: {r.stderr.strip()}")
    return r.stdout


def import_names(imports_dir: Path) -> dict:
    """Per module, the names it resolves in its siblings (kImportNames of imports/<mod>.cpp)."""
    names = {}
    for f in imports_dir.glob("*.cpp"):
        own = names.setdefault(f.stem, set())
        inside = False
        for line in f.read_text().splitlines():
            if line.startswith("static const char* const kImportNames"):
                inside = True
            elif inside and line == "};":
                break
            elif inside:
                m = IMPORT_NAME.match(line)
                if m and m.group(1):
                    own.add(m.group(1))
    return names


def defined_globals(nm: str, obj: Path) -> list:
    out = run([nm, "-P", "--defined-only", "-g", str(obj)])
    return [line.split()[0] for line in out.splitlines() if line.split()]


def sections(readelf: str, obj: Path) -> list:
    out = run([readelf, "-SW", str(obj)])
    names = []
    for line in out.splitlines():
        m = re.match(r"\s*\[\s*\d+\]\s+(\S+)\s+(\S+)", line)
        if m and m.group(1) != "NULL":
            names.append((m.group(1), m.group(2)))
    return names


def rename_for(name: str, mod: str):
    if name == ".data" or name.startswith(".data."):  # .data.rel.ro too: written by relocation only
        return f"re4d_{mod}"
    if name == ".bss" or name.startswith(".bss."):
        return f"re4b_{mod}"
    if name == ".init_array" or name.startswith(".init_array."):
        return f"re4i_{mod}"
    return None


def asm_name(s: str) -> str:
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--objects", type=Path, required=True, help="lines <mod>=<obj>;<obj>;...")
    ap.add_argument("--imports", type=Path, required=True, help="generated/imports of whole_link_plan.py")
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--ld", required=True, help="a linker that does `-r` (ld.lld, arm-none-eabi-ld)")
    ap.add_argument("--objcopy", required=True)
    ap.add_argument("--nm", required=True)
    ap.add_argument("--readelf", required=True)
    ap.add_argument("--wrap", action="append", default=[], help="symbols the module libraries wrap (_Znwj ...)")
    ap.add_argument("--ldscript", type=Path, help="write the linker script fragment for 3dsx.ld here")
    args = ap.parse_args()

    imports = import_names(args.imports)
    lookups = set().union(*imports.values())
    args.out.mkdir(parents=True, exist_ok=True)
    table = []
    for line in args.objects.read_text().splitlines():
        if not line.strip():
            continue
        mod, objs = line.split("=", 1)
        objs = [o for o in objs.split(";") if o]
        raw = args.out / f"{mod}.r.o"
        obj = args.out / f"{mod}.o"
        run([args.ld, "-r", "-o", str(raw), *objs])

        secs = sections(args.readelf, raw)
        bad = [n for n, t in secs if n.startswith((".tdata", ".tbss", ".fini_array", ".ctors", ".dtors", ".preinit_array"))]
        if bad:
            sys.exit(f"module {mod}: sections the static loader does not handle: {bad}")
        # Not the module's own import trampolines (imports/<mod>.S): re4_try_resolve_import would find one before
        # the real symbol in an older module, and it resolves to itself (the r22c shooting range hang, 2026-11-18).
        own = imports.get(mod, set())
        exports = sorted(s for s in defined_globals(args.nm, raw)
                         if (s in lookups and s not in own) or s in SPECIAL or s.startswith(SPECIAL_PREFIX))
        keep = args.out / f"{mod}.keep"
        keep.write_text("".join(s + "\n" for s in exports))
        # 1: no groups, only the looked-up symbols global; 2: their names and the sections
        run([args.objcopy, "-R", ".group", f"--keep-global-symbols={keep}", str(raw), str(obj)])
        redef = args.out / f"{mod}.redef"
        redef.write_text("".join(f"{s} re4s_{mod}__{s}\n" for s in exports)
                         + "".join(f"{w} __wrap_{w}\n" for w in args.wrap))
        renames = []
        kinds = set()
        for n, t in secs:
            new = rename_for(n, mod)
            if new and not n.startswith(".rel"):
                renames.append(f"--rename-section={n}={new}")
                kinds.add(new.split("_", 1)[0])
        run([args.objcopy, f"--redefine-syms={redef}", *renames, str(obj)])
        raw.unlink()
        table.append((mod, exports, kinds))

    out = [
        "// Generated by port/tools/whole_static_modules.py: the REL modules linked into the",
        "// program image (RE4_STATIC_MODULES) and what the loader needs of each.",
        "#include \"re4_runtime.h\"",
        "",
    ]
    for mod, exports, kinds in table:
        for i, s in enumerate(exports):
            out.append(f"extern \"C\" char re4s_{mod}_{i} __asm__({asm_name(f're4s_{mod}__{s}')});")
        for k in ("re4d", "re4b"):
            if k in kinds:
                out.append(f"extern \"C\" char __start_{k}_{mod}[], __stop_{k}_{mod}[];")
        if "re4i" in kinds:
            out.append(f"extern \"C\" void (*__start_re4i_{mod}[])(), (*__stop_re4i_{mod}[])();")
        out.append(f"static const Re4StaticSymbol kSymbols_{mod}[] = {{")
        out += [f"    {{{asm_name(s)}, &re4s_{mod}_{i}}}," for i, s in enumerate(exports)]
        out += ["    {0, 0},", "};", ""]
    out.append(f"static const Re4StaticModule kStaticModules[] = {{")
    for mod, exports, kinds in table:
        d = f"__start_re4d_{mod}, __stop_re4d_{mod}" if "re4d" in kinds else "0, 0"
        b = f"__start_re4b_{mod}, __stop_re4b_{mod}" if "re4b" in kinds else "0, 0"
        i = f"__start_re4i_{mod}, __stop_re4i_{mod}" if "re4i" in kinds else "0, 0"
        out.append(f"    {{{asm_name(mod)}, kSymbols_{mod}, {i}, {d}, {b}}},")
    out += ["};", "",
            "__attribute__((constructor)) static void registerStaticModules()",
            "{",
            f"    re4_static_modules_register(kStaticModules, {len(table)});",
            "}", ""]
    text = "\n".join(out)
    path = args.out / "static_modules.cpp"
    if not path.exists() or path.read_text() != text:
        path.write_text(text)
    if args.ldscript:
        ld = ["/* Generated by port/tools/whole_static_modules.py: the sections of the static REL modules",
              "   inside the data segment of 3dsx.ld. */", "SECTIONS", "{"]
        for mod, _, kinds in table:
            for k in ("re4d", "re4i"):
                if k in kinds:
                    ld.append(f"    {k}_{mod} : {{ KEEP(*({k}_{mod})) }}")
        ld += ["}", "INSERT AFTER .dtors;", "SECTIONS", "{"]
        ld += [f"    re4b_{mod} (NOLOAD) : {{ *(re4b_{mod}) }}" for mod, _, kinds in table if "re4b" in kinds]
        ld += ["}", "INSERT AFTER .bss;", ""]
        text = "\n".join(ld)
        if not args.ldscript.exists() or args.ldscript.read_text() != text:
            args.ldscript.write_text(text)
    print(f"static modules: {len(table)}, symbols looked up by name: {sum(len(e) for _, e, _ in table)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
