#!/usr/bin/env python3
"""Compare the export CRCs of a kernel build with a reference and say which modules would no longer load.

usage: crc-compare.py <build dir or Module.symvers> <reference Module.symvers> [--used DIR ...] [--list N]

  Module.symvers lines: <crc>\t<symbol>\t<module>\t<export type>\t<namespace>

  The reference is either the table recovered from the debug kernel (gda, what this project boots: a rebuilt kernel must reproduce it exactly), or the
  Module.symvers of a previous build (to see what a config change does to the ABI).
  A symbol is "changed" when its CRC differs or when the reference exports it and the build does not.
  --used DIR  (repeatable) every .ko in DIR: its imports (the __versions section) are checked against the changed symbols; a module that imports one
              will be refused by the kernel ("disagrees about version of symbol").  Imports that come from other modules (vendor modules export to each other)
              are not in a kernel Module.symvers and are not judged here.
"""
import os
import struct
import sys


def read_symvers(path):
    if os.path.isdir(path):
        path = os.path.join(path, "Module.symvers")
    d = {}
    for line in open(path, encoding="utf-8", errors="replace"):
        f = line.rstrip("\n").split("\t")
        if len(f) >= 3:
            d[f[1]] = (int(f[0], 16) & 0xffffffff, f[2])
    return d


def ko_imports(path):
    """symbol -> crc from the __versions section of an ELF64 little-endian .ko"""
    data = open(path, "rb").read()
    shoff, = struct.unpack_from("<Q", data, 0x28)
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", data, 0x3A)
    secs = []
    for i in range(shnum):
        name, typ, flags, addr, off, size = struct.unpack_from("<IIQQQQ", data, shoff + i * shentsize)
        secs.append((name, off, size))
    stroff = secs[shstrndx][1]
    for name, off, size in secs:
        if data[stroff + name:data.index(b"\0", stroff + name)] == b"__versions":
            out = {}
            for p in range(off, off + size, 64):
                crc, = struct.unpack_from("<Q", data, p)
                out[data[p + 8:p + 64].split(b"\0")[0].decode()] = crc & 0xffffffff
            return out
    return {}


def main():
    args = [a for i, a in enumerate(sys.argv[1:], 1) if not a.startswith("--") and sys.argv[i - 1] not in ("--used", "--list")]
    ours = read_symvers(args[0])
    ref = read_symvers(args[1])
    nlist = int(sys.argv[sys.argv.index("--list") + 1]) if "--list" in sys.argv else 25
    used = [sys.argv[i + 1] for i, a in enumerate(sys.argv) if a == "--used"]
    both = [s for s in ref if s in ours]
    diff = sorted(s for s in both if ref[s][0] != ours[s][0])
    removed = sorted(s for s in ref if s not in ours)
    added = [s for s in ours if s not in ref]
    print("reference exports %d, build exports %d, in both %d: CRC differs %d, removed %d, new %d" %
          (len(ref), len(ours), len(both), len(diff), len(removed), len(added)))
    for s in diff[:nlist]:
        print("  DIFF    %-45s ref %08x  build %08x  (%s)" % (s, ref[s][0], ours[s][0], ours[s][1]))
    for s in removed[:nlist]:
        print("  REMOVED %-45s (%s)" % (s, ref[s][1]))
    changed = set(diff) | set(removed)
    if not used:
        return
    hit = {}
    count = 0
    for d in used:
        for root, _, names in os.walk(d):
            for n in sorted(names):
                if n.endswith(".ko"):
                    count += 1
                    for sym in ko_imports(os.path.join(root, n)):
                        if sym in changed:
                            hit.setdefault(sym, []).append(n)
    print("checked %d modules: %d changed symbols are imported by at least one of them" % (count, len(hit)))
    for s in sorted(hit)[:nlist]:
        print("  USED    %-45s by %d: %s" % (s, len(hit[s]), ", ".join(hit[s][:5])))


main()
