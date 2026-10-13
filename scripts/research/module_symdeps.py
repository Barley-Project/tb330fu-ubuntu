#!/usr/bin/env python3
"""Symbol-level dependency map of the stock gda modules (read-only).

 * provider[sym]  = the stock module that exports sym (via __ksymtab_<sym> symbols)
 * for each requested module (and optionally its transitive closure) print, per provider module, which symbols it imports

usage: 260_module_symdeps.py <modules dir> <module> [<module> ...]      (names without .ko)
env:   CLOSURE=1  also walk the dependency closure and print a load order (dependency-first) plus the full import table
"""
import glob
import os
import sys

from elftools.elf.elffile import ELFFile

moddir = sys.argv[1]
targets = sys.argv[2:]
closure = os.environ.get("CLOSURE") == "1"

exports = {}      # module -> set(sym)
imports = {}      # module -> set(sym)
provider = {}     # sym -> module
for p in sorted(glob.glob(os.path.join(moddir, "*.ko"))):
    name = os.path.basename(p)[:-3]
    try:
        with open(p, "rb") as f:
            e = ELFFile(f)
            st = e.get_section_by_name(".symtab")
            ex, im = set(), set()
            for s in st.iter_symbols():
                if s.name.startswith("__ksymtab_") and s["st_shndx"] != "SHN_UNDEF":
                    ex.add(s.name[len("__ksymtab_"):])
                elif s["st_shndx"] == "SHN_UNDEF" and s.name and not s.name.startswith("__ksymtab"):
                    im.add(s.name)
    except Exception as ex_:
        print("skip", name, ex_)
        continue
    exports[name], imports[name] = ex, im
    for sym in ex:
        provider.setdefault(sym, name)


def deps_of(m):
    out = {}
    for sym in imports.get(m, ()):
        pm = provider.get(sym)
        if pm and pm != m:
            out.setdefault(pm, set()).add(sym)
    return out


order, seen = [], set()


def visit(m, depth=0):
    if m in seen:
        return
    seen.add(m)
    for pm in sorted(deps_of(m)):
        visit(pm, depth + 1)
    order.append(m)


for t in targets:
    d = deps_of(t)
    print("=== %s imports from other stock modules:" % t)
    for pm in sorted(d):
        print("   %-26s %2d symbols: %s" % (pm, len(d[pm]), " ".join(sorted(d[pm]))[:300]))
    if closure:
        visit(t)
if closure:
    print()
    print("dependency-first load order for the closure (%d modules):" % len(order))
    print(" ".join(order))
