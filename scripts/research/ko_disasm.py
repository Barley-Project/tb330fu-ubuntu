#!/usr/bin/env python3
"""Read-only AArch64 kernel-module (ET_REL .ko) function disassembler with relocation resolution.

 * disassembles one function symbol from its section
 * resolves CALL26/JUMP26 to symbol names, ADRP/ADD/LDR page-offset pairs to "section+off" and, for string sections, prints
   the C string
usage: 180_ko_disasm.py <module.ko> <function> [--max N]
       180_ko_disasm.py <module.ko> --list      (function symbols)
"""
import struct
import sys

from capstone import CS_ARCH_ARM64, CS_MODE_ARM, Cs
from elftools.elf.elffile import ELFFile
from elftools.elf.relocation import RelocationSection

path = sys.argv[1]
f = open(path, "rb")
elf = ELFFile(f)
symtab = elf.get_section_by_name(".symtab")
syms = list(symtab.iter_symbols())

if sys.argv[2] == "--list":
    for s in syms:
        if s["st_info"]["type"] == "STT_FUNC":
            print("%-40s sec=%d size=%d" % (s.name, s["st_shndx"], s["st_size"]))
    sys.exit(0)

fname = sys.argv[2]
maxn = int(sys.argv[sys.argv.index("--max") + 1]) if "--max" in sys.argv else 100000
target = [s for s in syms if s.name == fname and s["st_info"]["type"] == "STT_FUNC"]
if not target:
    print("function not found")
    sys.exit(1)
fs = target[0]
sec = elf.get_section(fs["st_shndx"])
data = sec.data()
off, size = fs["st_value"], fs["st_size"]
code = data[off:off + size]

# relocations that apply to this section
relocs = {}
for rs in elf.iter_sections():
    if isinstance(rs, RelocationSection) and rs.header["sh_info"] == fs["st_shndx"]:
        for r in rs.iter_relocations():
            o = r["r_offset"]
            if off <= o < off + size:
                s = syms[r["r_info_sym"]]
                relocs[o] = (r["r_info_type"], s, r["r_addend"])


def cstr_at(secname_or_idx, o):
    try:
        sc = elf.get_section(secname_or_idx) if isinstance(secname_or_idx, int) else elf.get_section_by_name(secname_or_idx)
        d = sc.data()
        e = d.index(b"\0", o)
        s = d[o:e]
        if 2 <= len(s) <= 200 and all(32 <= c < 127 or c in (9, 10) for c in s):
            return s.decode()
    except Exception:
        pass
    return None


md = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
md.detail = False
TYPES = {257: "ABS64", 275: "ADR_PREL_PG_HI21", 277: "ADD_ABS_LO12_NC", 278: "LDST8_ABS_LO12_NC", 284: "LDST16", 285: "LDST32_ABS_LO12_NC", 286: "LDST64_ABS_LO12_NC", 282: "JUMP26", 283: "CALL26", 310: "ADR_GOT_PAGE?", 311: "LD64_GOT_LO12_NC"}
print("%s :: %s  (%d bytes, section %s)" % (path.replace("\\", "/").split("/")[-1], fname, size, sec.name))
n = 0
for i in md.disasm(code, off):
    ann = ""
    if i.address in relocs:
        t, s, a = relocs[i.address]
        sname = s.name if s.name else (elf.get_section(s["st_shndx"]).name if isinstance(s["st_shndx"], int) and s["st_shndx"] < elf.num_sections() else "?")
        ann = "  ; R_%s -> %s%+d" % (TYPES.get(t, t), sname, a)
        if t in (275, 277, 285, 286, 278):
            secname = elf.get_section(s["st_shndx"]).name if isinstance(s["st_shndx"], int) and s["st_shndx"] < elf.num_sections() else ""
            if "str" in secname or "rodata" in secname:
                cs = cstr_at(s["st_shndx"], (s["st_value"] + a) if t in (277, 275) else 0)
                if cs and t in (277,):
                    ann += '  "%s"' % cs.replace("\n", "\\n")
    print("%06x  %-8s %-34s%s" % (i.address, i.mnemonic, i.op_str, ann))
    n += 1
    if n >= maxn:
        break
