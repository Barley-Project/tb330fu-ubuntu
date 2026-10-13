#!/usr/bin/env python3
"""kCFI check for the callback types of tb330fu_con.ko that scripts/243 does not cover.

The kernel is built with kCFI: every function that is called through a pointer starts with a 4 byte type hash, and the call site
compares the hash in front of the target with the hash of the pointer type.  A mismatch is a BUG()/panic at the first call.
tb330fu_con.ko registers a struct console whose .write has the type  void (*)(struct console *, const char *, unsigned int);
the gda kernel has several drivers with exactly this type (univ8250_console_write, pl011_console_write, pstore_console_write, ...), so the
hash in front of those kernel functions IS the hash the kernel's call site expects.

usage: con_cfi.py <module.ko> <System.map> <kernel raw Image> <write-callback-name> [<other-name-expected-hash-source> ...]
"""
import struct
import sys

from elftools.elf.elffile import ELFFile

ko, sysmap, image, cbname = sys.argv[1:5]
REF = ["univ8250_console_write", "pl011_console_write", "pstore_console_write", "hvc_console_print", "s3c24xx_serial_console_write",
       "qcom_geni_serial_console_write", "tegra_tcu_console_write", "sprd_console_write"]

sm = {}
for line in open(sysmap, encoding="utf-8", errors="replace"):
    p = line.split()
    if len(p) >= 3:
        sm[p[2]] = int(p[0], 16)
img = open(image, "rb").read()
base = sm["_text"]
print("kernel reference functions with the type  void (struct console *, const char *, unsigned int):")
refs = {}
for r in REF:
    if r in sm:
        refs[r] = struct.unpack_from("<I", img, sm[r] - base - 4)[0]
        print("  %-34s @%x  kCFI hash %08x" % (r, sm[r], refs[r]))
ok = len(set(refs.values())) == 1 and len(refs) >= 3
print("%s  %d kernel functions of that type agree on one hash" % ("PASS" if ok else "FAIL", len(refs)))
want = next(iter(refs.values())) if ok else None

e = ELFFile(open(ko, "rb"))
sym = [s for s in e.get_section_by_name(".symtab").iter_symbols() if s.name == cbname][0]
sec = e.get_section(sym["st_shndx"])
mine = struct.unpack_from("<I", sec.data(), sym["st_value"] - 4)[0]
print("module %s: %s kCFI hash %08x (section %s)" % (ko.split("/")[-1], cbname, mine, sec.name))
good = ok and mine == want
print("%s  module callback hash == the kernel's hash for the console write type" % ("PASS" if good else "FAIL"))
sys.exit(0 if good else 1)
