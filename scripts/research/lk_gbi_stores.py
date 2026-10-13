#!/usr/bin/env python3
"""Read-only: list code that writes/reads fields of g_boot_info (VA 0x4c546c5c) in the CN LK.

usage: python -I lk_gbi_stores.py <lk.img> <field_hex> [<field_hex> ...]
Prints the load site (PIC add) and every ldr/str [reg, #field] within the next 40 insns.
"""
import re
import struct
import sys

from capstone import CS_ARCH_ARM, CS_MODE_THUMB, Cs

BASE, HDR, GBI = 0x4C400000, 0x200, 0x4C546C5C


def main():
    path = sys.argv[1]
    fields = [int(x, 16) for x in sys.argv[2:]]
    d = open(path, "rb").read()
    dsize = struct.unpack_from("<I", d, 4)[0]
    pay = d[HDR:HDR + dsize]
    md = Cs(CS_ARCH_ARM, CS_MODE_THUMB)
    md.skipdata = True
    ins = list(md.disasm(pay[:0x9A000], BASE))
    pend = {}
    holders = []  # (idx, reg)
    for idx, i in enumerate(ins):
        m = i.mnemonic
        if m.startswith("ldr") and "[pc" in i.op_str:
            mm = re.match(r"(\w+), \[pc(?:, #(-?0x[0-9a-f]+|-?\d+))?\]", i.op_str)
            if mm:
                reg = mm.group(1)
                imm = int(mm.group(2), 0) if mm.group(2) else 0
                la = ((i.address + 4) & ~3) + imm
                o = la - BASE
                if 0 <= o + 4 <= len(pay):
                    pend[reg] = (idx, struct.unpack_from("<I", pay, o)[0])
        elif m.startswith("add") and i.op_str.endswith(", pc"):
            reg = i.op_str.split(",")[0].strip()
            if reg in pend and idx - pend[reg][0] < 12:
                tgt = (pend[reg][1] + i.address + 4) & 0xFFFFFFFF
                if tgt == GBI:
                    holders.append((idx, reg, i.address))
    print("g_boot_info (0x%08x) address materialised at %d places" % (GBI, len(holders)))
    for fld in fields:
        print("=" * 90)
        print("accesses to g_boot_info+0x%x" % fld)
        pat_s = re.compile(r"^(\w+), \[(\w+), #%s\]$" % hex(fld))
        for idx, reg, addr in holders:
            for j in range(idx + 1, min(idx + 60, len(ins))):
                x = ins[j]
                mm = pat_s.match(x.op_str)
                if mm and mm.group(2) == reg and (x.mnemonic.startswith("str") or x.mnemonic.startswith("ldr")):
                    print("  0x%08x  %-5s %-22s (g_boot_info loaded at 0x%08x into %s)" % (x.address, x.mnemonic, x.op_str, addr, reg))
                    break
                # stop when the register is clobbered by a new ldr/mov/add into it
                if x.op_str.startswith(reg + ",") and x.mnemonic.split(".")[0] in ("mov", "movs", "movw", "ldr", "adds", "add", "sub", "subs") and "pc" not in x.op_str:
                    pass


if __name__ == "__main__":
    main()
