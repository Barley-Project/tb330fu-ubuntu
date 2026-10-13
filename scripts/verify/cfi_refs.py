#!/usr/bin/env python3
"""kCFI check by reference: the hash in front of a module function that is called through a pointer must equal the hash in front of kernel functions of
exactly the same prototype (the kernel's call sites compare against that hash).

usage: cfi_refs.py <module.ko> <System.map> <kernel raw Image> <modfn>=<ref1>,<ref2>,... [<modfn>=...]
exit 0 if every module function's hash equals the (unanimous) hash of its references
"""
import struct
import sys

from elftools.elf.elffile import ELFFile

ko, sysmap, image = sys.argv[1:4]
pairs = [a.split("=", 1) for a in sys.argv[4:]]
sm = {}
for line in open(sysmap, encoding="utf-8", errors="replace"):
    p = line.split()
    if len(p) >= 3:
        sm.setdefault(p[2], int(p[0], 16))
img = open(image, "rb").read()
base = sm["_text"]
e = ELFFile(open(ko, "rb"))
syms = {}
for s in e.get_section_by_name(".symtab").iter_symbols():
    if s.name and isinstance(s["st_shndx"], int):
        syms.setdefault(s.name, s)
bad = 0
for fn, refs in pairs:
    refs = refs.split(",")
    rh = {}
    for r in refs:
        if r in sm:
            rh[r] = struct.unpack_from("<I", img, sm[r] - base - 4)[0]
    s = syms.get(fn)
    if s is None:
        print("FAIL  %s: not found in the module" % fn)
        bad += 1
        continue
    sec = e.get_section(s["st_shndx"])
    mine = struct.unpack_from("<I", sec.data(), s["st_value"] - 4)[0]
    ok = len(rh) >= 2 and len(set(rh.values())) == 1 and mine in rh.values()
    print("%s  %-24s kCFI %08x  references %s" % ("PASS" if ok else "FAIL", fn, mine, ", ".join("%s=%08x" % kv for kv in rh.items()) or "NONE FOUND"))
    bad += 0 if ok else 1
sys.exit(1 if bad else 0)
