#!/usr/bin/env python3
"""Write a Module.symvers fragment (for KBUILD_EXTRA_SYMBOLS) with symbols that a vendor module exports.

usage: module-symvers.py <vendor module.ko> <out.symvers> <symbol> [<symbol> ...]
Used to build modules that call into vendor modules (e.g. tb330fu_ppmtemp -> mtk_ppm_v3): the CRC that the importing module must carry is the one stored
in the exporting module (__crc_<symbol>).
"""
import struct
import sys

from elftools.elf.elffile import ELFFile

ko, out, names = sys.argv[1], sys.argv[2], sys.argv[3:]
f = open(ko, "rb")
e = ELFFile(f)
symtab = e.get_section_by_name(".symtab")
crc = {}
for s in symtab.iter_symbols():
    if s.name.startswith("__crc_") and isinstance(s["st_shndx"], int):
        sec = e.get_section(s["st_shndx"])
        data = sec.data()
        if s["st_value"] + 4 <= len(data):
            crc[s.name[6:]] = struct.unpack_from("<I", data, s["st_value"])[0]
modname = ko.rsplit("/", 1)[-1][:-3]
with open(out, "w") as o:
    for n in names:
        if n not in crc:
            sys.exit("%s does not export %s" % (ko, n))
        o.write("0x%08x\t%s\t%s\tEXPORT_SYMBOL\t\n" % (crc[n], n, modname))
print("extra.symvers: %d entries from %s" % (len(names), modname))
