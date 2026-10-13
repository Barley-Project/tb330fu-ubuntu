#!/usr/bin/env python3
"""Read-only simulation of the kernel's module loader for a whole load sequence (what `insmod` would check, in order).

For every module in the sequence, in order:
  * vermagic must equal the gda kernel's
  * every import in __versions must be exported either by the gda kernel (boot-debug-exports.json, CRC compared; GPL-only
    symbols need a GPL-compatible license) or by a module loaded EARLIER in the sequence (CRC compared)
  * the GKI protected-export list must not be touched (unsigned modules)
Prints the first problem per module and a summary.  Exit 1 if anything fails.

usage: loadsim.py <sequence-file> [<dir-with-ko>...]
  sequence-file: one module file name per line (e.g. bootprof.ko); '#' comments allowed.  Files are searched in the given dirs
  (first match wins), so put work/modbuild before the ramdisk modules dir to pick up the locally built modules.
"""
import json
import os
import struct
import sys

from elftools.elf.elffile import ELFFile

EXPORTS = os.environ.get("TB_EXPORTS") or os.path.join(os.environ.get("TB_KDATA", os.path.expanduser("~/tb330fu-work/kernel/data")), "boot-debug", "exports.json")
PROT = os.environ.get("TB_PROTECTED") or os.path.join(os.environ.get("TB_KSRC", os.path.expanduser("~/tb330fu-work/kernel/common")), "android", "abi_gki_protected_exports_aarch64")
GDA = "6.6.30-android15-8-gda510ce0fdcf-4k SMP preempt mod_unload modversions aarch64"

seqfile, dirs = sys.argv[1], sys.argv[2:]
kern = json.load(open(EXPORTS))
prot = set(l.strip() for l in open(PROT) if l.strip() and not l.startswith("#"))
seq = [l.strip() for l in open(seqfile) if l.strip() and not l.startswith("#")]


def find(name):
    for d in dirs:
        p = os.path.join(d, name)
        if os.path.exists(p):
            return p
    return None


def info(path):
    e = ELFFile(open(path, "rb"))
    mi = [x.decode("latin1") for x in e.get_section_by_name(".modinfo").data().split(b"\0") if x]
    vm = [x[9:] for x in mi if x.startswith("vermagic=")]
    lic = [x[8:] for x in mi if x.startswith("license=")]
    # imports with CRC
    s = e.get_section_by_name("__versions")
    imp = {}
    if s is not None:
        d = s.data()
        for i in range(0, len(d), 64):
            crc, = struct.unpack_from("<Q", d, i)
            n = d[i + 8:i + 64].split(b"\0")[0].decode()
            if n:
                imp[n] = crc & 0xffffffff
    # undefined symbols (to catch imports without a CRC entry)
    und = set(sy.name for sy in e.get_section_by_name(".symtab").iter_symbols() if sy["st_shndx"] == "SHN_UNDEF" and sy.name)
    # exports and their CRCs; GPL-only ones live in __kcrctab_gpl
    ex = {}
    for sy in e.get_section_by_name(".symtab").iter_symbols():
        if sy.name.startswith("__crc_") and isinstance(sy["st_shndx"], int):
            sec = e.get_section(sy["st_shndx"])
            ex[sy.name[6:]] = (struct.unpack_from("<I", sec.data(), sy["st_value"])[0], "gpl" in sec.name)
    return (vm[0] if vm else None), (lic[0] if lic else ""), imp, und, ex


provided = {}          # symbol -> (crc, gpl_only, module)
problems = 0
for idx, name in enumerate(seq):
    p = find(name)
    if p is None:
        print("FAIL  [%2d] %-34s file not found" % (idx, name))
        problems += 1
        continue
    vm, lic, imp, und, ex = info(p)
    errs = []
    if vm != GDA:
        errs.append("vermagic %r" % vm)
    for sym in sorted(und):
        if sym not in imp and sym not in ("module_layout",):
            # symbols resolved without modversions CRC (rare: e.g. __this_module, or declared with no version) - only report if neither side knows it
            if sym not in kern and sym not in provided and not sym.startswith("__this_module"):
                errs.append("undefined %s (no CRC entry, not exported anywhere)" % sym)
    for sym, crc in sorted(imp.items()):
        if sym in provided:
            pc, pg, pm = provided[sym]
            if pc != crc:
                errs.append("%s: crc %08x != %s's %08x" % (sym, crc, pm, pc))
            elif pg and "GPL" not in lic:
                errs.append("%s is GPL-only (from %s) but license is %r" % (sym, pm, lic))
        elif sym in kern:
            k = kern[sym]
            if (k["crc"] & 0xffffffff) != crc:
                errs.append("%s: crc %08x != kernel %08x" % (sym, crc, k["crc"] & 0xffffffff))
            elif k["gpl_only"] and "GPL" not in lic:
                errs.append("%s is GPL-only but license is %r" % (sym, lic))
            if sym in prot:
                errs.append("%s is a GKI protected export" % sym)
        else:
            errs.append("unknown symbol %s (not in the kernel, not exported by an earlier module)" % sym)
    for sym, (crc, g) in ex.items():
        provided.setdefault(sym, (crc, g, name))
    if errs:
        problems += 1
        print("FAIL  [%2d] %-34s %s" % (idx, name, "; ".join(errs[:4]) + (" ... (+%d more)" % (len(errs) - 4) if len(errs) > 4 else "")))
    else:
        print("ok    [%2d] %-34s imports %3d, exports %2d" % (idx, name, len(imp), len(ex)))
print()
print("%d modules simulated, %d with problems" % (len(seq), problems))
sys.exit(1 if problems else 0)
