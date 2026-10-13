#!/usr/bin/env python3
"""Read-only PIC-aware cross-reference finder for the CN LK payload (Thumb-2, base 0x4c400000).

usage: python -I lk_xref.py <lk.img> str:<substring> [str:...] [fn:<va_hex> ...]
  str:<s>  : find code that references any C-string containing <s>
  fn:<va>  : find all direct BL/B.W/BLX callers of function <va>
"""
import re
import struct
import sys

from capstone import CS_ARCH_ARM, CS_MODE_THUMB, Cs

BASE, HDR = 0x4C400000, 0x200


def main():
    path = sys.argv[1]
    d = open(path, "rb").read()
    dsize = struct.unpack_from("<I", d, 4)[0]
    pay = d[HDR:HDR + dsize]
    md = Cs(CS_ARCH_ARM, CS_MODE_THUMB)
    md.detail = False
    md.skipdata = True

    # linear sweep of the code region (up to where strings start ~0x9a000)
    CODE_END = 0x9A000
    ins = list(md.disasm(pay[:CODE_END], BASE))

    # PIC reference table:  addr_of_add -> target
    pic = {}      # code_addr (of the add) -> (target, reg)
    calls = {}    # callee -> [caller_addr]
    ldr_pending = {}
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
                    ldr_pending[reg] = (idx, struct.unpack_from("<I", pay, o)[0])
        elif m.startswith("add") and i.op_str.endswith(", pc"):
            reg = i.op_str.split(",")[0].strip()
            if reg in ldr_pending and idx - ldr_pending[reg][0] < 12:
                tgt = (ldr_pending[reg][1] + i.address + 4) & 0xFFFFFFFF
                pic[i.address] = (tgt, reg)
        elif m in ("bl", "b.w", "blx", "b"):
            mm = re.match(r"#(0x[0-9a-f]+)", i.op_str)
            if mm:
                calls.setdefault(int(mm.group(1), 16), []).append(i.address)

    def cstr_at(va):
        o = va - BASE
        if not (0 <= o < len(pay)):
            return None
        e = pay.find(b"\0", o)
        s = pay[o:e]
        return s.decode("latin1") if 1 <= len(s) <= 200 and all(32 <= c < 127 or c in (9, 10) for c in s) else None

    for q in sys.argv[2:]:
        kind, val = q.split(":", 1)
        print("=" * 80)
        if kind == "str":
            print("string refs matching %r" % val)
            seen = set()
            for a, (t, r) in sorted(pic.items()):
                s = cstr_at(t)
                if s and val in s:
                    # which function (nearest preceding push lr)?
                    print("  code 0x%08x  -> 0x%08x  %r" % (a, t, s.replace("\n", "\\n")))
        elif kind == "fn":
            fva = int(val, 16)
            cl = calls.get(fva, [])
            print("callers of 0x%08x: %s" % (fva, ", ".join("0x%08x" % c for c in cl) or "(none found)"))


if __name__ == "__main__":
    main()
