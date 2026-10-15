#!/usr/bin/env python3
"""Set the exported CRC of selected symbols in a built module to the value another module expects.

usage: patch-crc.py <in.ko> <out.ko> <importer.ko> <symbol> [<symbol> ...]

For stand-in modules (tb330fu_spmstub): they export a function of a vendor module whose exact prototype is not known, so the CRC that genksyms computes for the
stand-in's own prototype is not the one the importing vendor module carries in its __versions section.  The stand-in's behaviour does not depend on the
prototype (it only returns 0), so its CRC words (__kcrctab entries, found through the __crc_<symbol> symbols) are overwritten with the importer's.
"""
import struct
import sys

from elftools.elf.elffile import ELFFile

src, dst, importer = sys.argv[1:4]
names = sys.argv[4:]

want = {}
with open(importer, "rb") as f:
    e = ELFFile(f)
    sec = e.get_section_by_name("__versions")
    d = sec.data()
    for p in range(0, len(d), 64):
        crc, = struct.unpack_from("<Q", d, p)
        want[d[p + 8:p + 64].split(b"\0")[0].decode()] = crc & 0xffffffff
data = bytearray(open(src, "rb").read())
with open(src, "rb") as f:
    e = ELFFile(f)
    st = e.get_section_by_name(".symtab")
    where = {}
    for s in st.iter_symbols():
        if s.name.startswith("__crc_") and isinstance(s["st_shndx"], int):
            sec = e.get_section(s["st_shndx"])
            where[s.name[6:]] = sec["sh_offset"] + s["st_value"]
for n in names:
    if n not in want:
        sys.exit("%s does not import %s" % (importer, n))
    if n not in where:
        sys.exit("%s does not export %s" % (src, n))
    old, = struct.unpack_from("<I", data, where[n])
    struct.pack_into("<I", data, where[n], want[n])
    print("%s: CRC %08x -> %08x" % (n, old, want[n]))
open(dst, "wb").write(data)
