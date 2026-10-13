#!/usr/bin/env python3
"""Read-only: show every call site of an imported symbol in an AArch64 kernel module (ET_REL .ko).

For each R_AARCH64_CALL26/JUMP26 relocation against <symbol>: name of the containing function, offset, and a disassembly window
(--before N / --after M instructions) with relocation annotations resolved.

usage: 261_callsites.py <module.ko> <symbol> [--before 24] [--after 24] [--max 6]
"""
import sys

from capstone import CS_ARCH_ARM64, CS_MODE_ARM, Cs
from elftools.elf.elffile import ELFFile
from elftools.elf.relocation import RelocationSection

path, symname = sys.argv[1], sys.argv[2]
before = int(sys.argv[sys.argv.index("--before") + 1]) if "--before" in sys.argv else 24
after = int(sys.argv[sys.argv.index("--after") + 1]) if "--after" in sys.argv else 24
maxn = int(sys.argv[sys.argv.index("--max") + 1]) if "--max" in sys.argv else 6

f = open(path, "rb")
elf = ELFFile(f)
syms = list(elf.get_section_by_name(".symtab").iter_symbols())
funcs = {}
for s in syms:
    if s["st_info"]["type"] == "STT_FUNC" and s["st_size"]:
        funcs.setdefault(s["st_shndx"], []).append((s["st_value"], s["st_size"], s.name))
for k in funcs:
    funcs[k].sort()

target_idx = {i for i, s in enumerate(syms) if s.name == symname}
md = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
md.detail = False
TYPES = {257: "ABS64", 275: "ADR_PREL_PG_HI21", 277: "ADD_ABS_LO12_NC", 278: "LDST8", 284: "LDST16", 285: "LDST32", 286: "LDST64", 282: "JUMP26", 283: "CALL26"}
n = 0
for rs in elf.iter_sections():
    if not isinstance(rs, RelocationSection):
        continue
    tsec = elf.get_section(rs.header["sh_info"])
    if tsec is None or not tsec.name.startswith(".text"):
        continue
    data = tsec.data()
    relocs_here = {}
    for r in rs.iter_relocations():
        relocs_here[r["r_offset"]] = (r["r_info_type"], syms[r["r_info_sym"]], r["r_addend"])
    for off, (t, s, a) in sorted(relocs_here.items()):
        if t in (282, 283) and relocs_here[off][1].name == symname:
            fn = next(((v, sz, nm) for v, sz, nm in funcs.get(rs.header["sh_info"], []) if v <= off < v + sz), None)
            print("=" * 100)
            print("call to %s at %s+%#x  (section %s, offset %#x)" % (symname, fn[2] if fn else "?", off - (fn[0] if fn else 0), tsec.name, off))
            lo = max(fn[0] if fn else 0, off - before * 4)
            hi = min(len(data), off + after * 4 + 4)
            for i in md.disasm(data[lo:hi], lo):
                ann = ""
                if i.address in relocs_here:
                    t2, s2, a2 = relocs_here[i.address]
                    nm = s2.name or (elf.get_section(s2["st_shndx"]).name if isinstance(s2["st_shndx"], int) else "?")
                    ann = "   ; R_%s -> %s%+d" % (TYPES.get(t2, t2), nm, a2)
                mark = "  <== CALL" if i.address == off else ""
                print("%06x  %-8s %-34s%s%s" % (i.address, i.mnemonic, i.op_str, ann, mark))
            n += 1
            if n >= maxn:
                sys.exit(0)
if n == 0:
    print("no call sites found (symbol referenced only through data/relocs other than CALL26/JUMP26?)")
