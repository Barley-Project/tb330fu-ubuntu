#!/usr/bin/env python3
"""Annotate an llvm-objdump -dr listing of a relocatable kernel module: relocations against .rodata.str1.1+off are resolved to the C string.
usage: annotate_ko_dis.py <module.ko> <in.dis> <out.dis>"""
import re
import sys

from elftools.elf.elffile import ELFFile

ko, din, dout = sys.argv[1:4]
e = ELFFile(open(ko, "rb"))
sec = {}
for s in e.iter_sections():
    if s.name.startswith(".rodata.str1") or s.name == ".rodata":
        sec[s.name] = s.data()
out = []
for line in open(din, encoding="utf-8", errors="replace"):
    line = line.rstrip("\n")
    m = re.search(r"R_AARCH64_ADD_ABS_LO12_NC\s+(\.rodata[.\w]*)\+0x([0-9a-f]+)", line)
    if m and m.group(1) in sec:
        d = sec[m.group(1)]
        off = int(m.group(2), 16)
        end = d.find(b"\0", off)
        txt = d[off:end].decode("utf-8", "replace")
        if len(txt) < 200:
            line += '    ; "%s"' % txt.replace("\n", "\\n")
    out.append(line)
open(dout, "w", encoding="utf-8").write("\n".join(out) + "\n")
print("wrote", dout, len(out))
