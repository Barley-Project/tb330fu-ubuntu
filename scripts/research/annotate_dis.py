#!/usr/bin/env python3
"""Annotate an llvm-objdump AArch64 disassembly of an Android executable: resolve adrp/add pairs that point into .rodata (file offset == address there) to the
C string they reference, and name PLT calls.   usage: annotate_dis.py <binary> <in.dis> <out.dis>"""
import re
import sys

binf, din, dout = sys.argv[1:4]
data = open(binf, "rb").read()


def cstr(off):
    if off < 0 or off >= len(data):
        return None
    e = data.find(b"\0", off)
    s = data[off:e]
    if len(s) < 2 or any(c < 9 or (13 < c < 32) or c > 126 for c in s):
        return None
    return s.decode()


adrp = {}
out = []
for line in open(din):
    line = line.rstrip("\n")
    m = re.search(r"\badrp\s+(x\d+), (0x[0-9a-f]+)", line)
    if m:
        adrp[m.group(1)] = int(m.group(2), 16)
    m = re.search(r"\badd\s+(x\d+), (x\d+), #(0x[0-9a-f]+)", line)
    if m and m.group(2) in adrp:
        s = cstr(adrp[m.group(2)] + int(m.group(3), 16))
        if s is not None:
            line += '    ; "%s"' % s.replace("\n", "\\n")
    out.append(line)
open(dout, "w").write("\n".join(out) + "\n")
print("wrote", dout, len(out), "lines")
