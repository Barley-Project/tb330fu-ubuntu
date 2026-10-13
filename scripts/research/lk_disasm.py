#!/usr/bin/env python3
"""Read-only Thumb-2 disassembler helper for the CN LK payload (base 0x4c400000).

usage: python -I lk_disasm.py <lk.img> <start_va_hex> <end_va_hex> [--func]
  * prints instructions with PIC string/data references resolved
    (ldr rX,[pc,#i] + add rX,pc  ->  target address, and the C-string there, if any)
  * --func : back up from start_va to the nearest plausible function prologue
"""
import re
import struct
import sys

from capstone import CS_ARCH_ARM, CS_MODE_THUMB, Cs

BASE, HDR = 0x4C400000, 0x200


def main():
    path = sys.argv[1]
    start = int(sys.argv[2], 16)
    end = int(sys.argv[3], 16)
    d = open(path, "rb").read()
    dsize = struct.unpack_from("<I", d, 4)[0]
    pay = d[HDR:HDR + dsize]
    md = Cs(CS_ARCH_ARM, CS_MODE_THUMB)
    md.detail = True

    def cstr(va):
        o = va - BASE
        if not (0 <= o < len(pay)):
            return None
        e = pay.find(b"\0", o)
        s = pay[o:e]
        if 2 <= len(s) <= 120 and all(32 <= c < 127 or c in (9, 10) for c in s):
            return s.decode()
        return None

    if "--func" in sys.argv:
        best = None
        for s in range(start - 0x600, start, 2):
            ins = list(md.disasm(pay[s - BASE:start - BASE + 8], s))
            if not ins:
                continue
            first = ins[0]
            if not (first.mnemonic in ("push", "push.w") and "lr" in first.op_str):
                continue
            addrs = [i.address for i in ins]
            if start in addrs:
                # contiguous (no capstone gaps)
                if ins[-1].address + ins[-1].size >= start:
                    best = s
        if best:
            print("; function prologue guess at 0x%08x" % best)
            start = best

    ins = list(md.disasm(pay[start - BASE:end - BASE], start))
    # literal-pool aware PIC resolution
    by_addr = {i.address: i for i in ins}
    last_ldr = {}
    for i in ins:
        note = ""
        m = i.mnemonic
        if m.startswith("ldr") and "[pc" in i.op_str:
            mm = re.match(r"(\w+), \[pc(?:, #(-?0x[0-9a-f]+|-?\d+))?\]", i.op_str)
            if mm:
                reg = mm.group(1)
                imm = int(mm.group(2), 0) if mm.group(2) else 0
                lit_addr = ((i.address + 4) & ~3) + imm
                o = lit_addr - BASE
                if 0 <= o + 4 <= len(pay):
                    val = struct.unpack_from("<I", pay, o)[0]
                    last_ldr[reg] = (i.address, val)
                    note = "; lit@0x%08x = 0x%08x" % (lit_addr, val)
        elif m.startswith("add") and i.op_str.endswith(", pc"):
            reg = i.op_str.split(",")[0].strip()
            if reg in last_ldr:
                tgt = (last_ldr[reg][1] + i.address + 4) & 0xFFFFFFFF
                s = cstr(tgt)
                note = "; => 0x%08x%s" % (tgt, (' "%s"' % s) if s else "")
        elif m in ("bl", "blx", "b", "b.w", "beq", "bne", "beq.w", "bne.w", "cbz", "cbnz"):
            pass
        print("%08x  %-10s %-34s %s" % (i.address, i.mnemonic, i.op_str, note))


if __name__ == "__main__":
    main()
