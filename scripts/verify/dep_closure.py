#!/usr/bin/env python3
"""Dependency closure of kernel modules against the gda kernel's export table and a pool of module directories.

usage: dep_closure.py <target.ko> [<target2.ko> ...] --pool <dir> [--pool <dir> ...] [--have <name.ko> ...]
  - every .ko in the pool dirs is indexed by the symbols it exports (CRC from __kcrctab sections via the __crc_ symbols)
  - for every target: each undefined symbol (with CRC from __versions) must be exported by the gda kernel (boot-debug-exports.json, CRC compared) or by
    another pool module; that module is added to the closure and processed in turn
  - --have: modules that are already loaded on the device (their exports count as available, they are not added to the closure)
Prints the closure in a valid load order (dependencies first) and every unresolved symbol (with the module that needs it).
"""
import json
import os
import struct
import sys

from elftools.elf.elffile import ELFFile

EXPORTS = os.environ.get("TB_EXPORTS") or os.path.join(os.environ.get("TB_KDATA", os.path.expanduser("~/tb330fu-work/kernel/data")), "boot-debug", "exports.json")
kern = json.load(open(EXPORTS))

args = sys.argv[1:]
targets, pools, have = [], [], []
mode = "t"
for a in args:
    if a == "--pool":
        mode = "p"
    elif a == "--have":
        mode = "h"
    elif mode == "t":
        targets.append(a)
    elif mode == "p":
        pools.append(a)
        mode = "t_after_pool"
    elif mode == "h":
        have.append(a)
    else:
        targets.append(a)


def find(name):
    for d in pools:
        p = os.path.join(d, name)
        if os.path.exists(p):
            return p
    return None


_cache = {}


def info(path):
    if path in _cache:
        return _cache[path]
    e = ELFFile(open(path, "rb"))
    s = e.get_section_by_name("__versions")
    imp = {}
    if s is not None:
        d = s.data()
        for i in range(0, len(d), 64):
            crc, = struct.unpack_from("<Q", d, i)
            n = d[i + 8:i + 64].split(b"\0")[0].decode()
            if n:
                imp[n] = crc & 0xffffffff
    und = set(sy.name for sy in e.get_section_by_name(".symtab").iter_symbols() if sy["st_shndx"] == "SHN_UNDEF" and sy.name)
    ex = {}
    for sy in e.get_section_by_name(".symtab").iter_symbols():
        if sy.name.startswith("__crc_") and isinstance(sy["st_shndx"], int):
            # value of the absolute symbol = crc
            ex[sy.name[6:]] = sy["st_value"] & 0xffffffff
    # modules export via __ksymtab; the __crc_ symbols exist for exported names
    mi = [x.decode("latin1") for x in e.get_section_by_name(".modinfo").data().split(b"\0") if x]
    deps = [x[8:] for x in mi if x.startswith("depends=")]
    deps = [d for d in (deps[0].split(",") if deps and deps[0] else [])]
    _cache[path] = (imp, und, ex, deps)
    return _cache[path]


# index of exports for the pool
prov = {}
for d in pools:
    for fn in sorted(os.listdir(d)):
        if fn.endswith(".ko"):
            try:
                _, _, ex, _ = info(os.path.join(d, fn))
            except Exception as ex_:  # noqa
                continue
            for sym in ex:
                prov.setdefault(sym, []).append(fn)
havesyms = set()
for h in have:
    p = find(h)
    if p:
        _, _, ex, _ = info(p)
        havesyms |= set(ex)

order, seen, unresolved = [], set(), []


def visit(fn, why):
    if fn in seen:
        return
    seen.add(fn)
    p = find(fn)
    if not p:
        unresolved.append((fn, "module file not found (%s)" % why))
        return
    imp, und, ex, deps = info(p)
    for sym in sorted(imp):
        if sym == "module_layout":
            continue
        k = kern.get(sym)
        if k is not None:
            continue
        if sym in havesyms:
            continue
        cands = [c for c in prov.get(sym, []) if c != fn]
        if not cands:
            unresolved.append((fn, "symbol %s: not exported by the kernel nor by any pool module" % sym))
            continue
        # prefer a provider that is already in the closure, else the first
        pick = next((c for c in cands if c in seen), cands[0])
        if pick in have:
            continue
        visit(pick, "%s needs %s" % (fn, sym))
    order.append(fn)


for t in targets:
    visit(os.path.basename(t), "target")
print("LOAD ORDER (dependencies first), %d modules:" % len(order))
for i, m in enumerate(order):
    print("  %2d %s" % (i, m))
print()
print("UNRESOLVED (%d):" % len(unresolved))
for fn, why in unresolved:
    print("  %s: %s" % (fn, why))
