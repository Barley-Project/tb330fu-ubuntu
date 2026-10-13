#!/usr/bin/env python3
"""Read-only: can this module be loaded by the gda debug kernel?

Checks every undefined symbol of the module against the gda kernel's own export table
(tb330fu-compat-project/analysis/kernel-symbols/boot-debug-exports.json: name -> crc, gpl_only, namespace) and, for symbols that are
not exported by the kernel, against the exports of modules in the same load list (given as extra .ko files, in load order).

usage: verify_ko.py <module.ko> [<earlier-loaded.ko> ...]
exit 0 = every import resolvable with the right CRC
"""
import json
import os
import struct
import sys

from elftools.elf.elffile import ELFFile

EXPORTS = os.environ.get("TB_EXPORTS") or os.path.join(os.environ.get("TB_KDATA", os.path.expanduser("~/tb330fu-work/kernel/data")), "boot-debug", "exports.json")
GDA = "6.6.30-android15-8-gda510ce0fdcf-4k SMP preempt mod_unload modversions aarch64"

ko = sys.argv[1]
earlier = sys.argv[2:]
kern = json.load(open(EXPORTS))


def modinfo(e):
    return [x.decode("latin1") for x in e.get_section_by_name(".modinfo").data().split(b"\0") if x]


def versions(e):
    s = e.get_section_by_name("__versions")
    out = {}
    if s is None:
        return out
    d = s.data()
    for i in range(0, len(d), 64):
        crc, = struct.unpack_from("<Q", d, i)
        n = d[i + 8:i + 64].split(b"\0")[0].decode()
        if n:
            out[n] = crc & 0xffffffff
    return out


def module_exports(path):
    e = ELFFile(open(path, "rb"))
    out = {}
    for s in e.get_section_by_name(".symtab").iter_symbols():
        if s.name.startswith("__crc_") and isinstance(s["st_shndx"], int):
            sec = e.get_section(s["st_shndx"])
            out[s.name[6:]] = struct.unpack_from("<I", sec.data(), s["st_value"])[0]
    return out


e = ELFFile(open(ko, "rb"))
mi = modinfo(e)
lic = [x for x in mi if x.startswith("license=")]
print("module:", ko)
print("modinfo:", [x for x in mi if x.split("=")[0] in ("name", "vermagic", "license", "depends")])
mine = versions(e)
extra = {}
for p in earlier:
    for k, v in module_exports(p).items():
        extra.setdefault(k, (v, p))
bad = 0
nk = nm = 0
for name, crc in sorted(mine.items()):
    if name == "module_layout":
        kv = kern.get("module_layout")
        ok = kv is not None and kv["crc"] == crc
        print("%s  module_layout %08x vs gda %s" % ("PASS" if ok else "FAIL", crc, ("%08x" % kv["crc"]) if kv else "-"))
        bad += 0 if ok else 1
        continue
    if name in kern:
        k = kern[name]
        ok = (k["crc"] & 0xffffffff) == crc
        gpl_ok = (not k["gpl_only"]) or any("GPL" in x for x in lic)
        if not ok or not gpl_ok:
            print("FAIL  %-44s module crc %08x, gda crc %08x, gpl_only=%s license=%s" % (name, crc, k["crc"] & 0xffffffff, k["gpl_only"], lic))
            bad += 1
        nk += 1
    elif name in extra:
        ok = extra[name][0] == crc
        print("%s  %-44s from %s (crc %08x vs %08x)" % ("PASS" if ok else "FAIL", name, extra[name][1].split("/")[-1], crc, extra[name][0]))
        bad += 0 if ok else 1
        nm += 1
    else:
        print("FAIL  %-44s not exported by the gda kernel nor by an earlier module" % name)
        bad += 1
print()
print("%d imports with CRC: %d resolved against the gda kernel, %d against earlier modules, %d problems" % (len(mine), nk, nm, bad))
sys.exit(1 if bad else 0)
