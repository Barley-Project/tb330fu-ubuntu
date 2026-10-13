#!/usr/bin/env python3
"""Write a Module.symvers fragment (for KBUILD_EXTRA_SYMBOLS) with exports that the debug kernel has but the production tree lacks.

usage: make-extra-symvers.py <exports.json of the target kernel> <out.symvers> <symbol> [<symbol> ...]
The CRCs come from the target kernel's own export table (scripts/kernel/extract_kernel_data.py), so a module that imports these symbols
carries exactly the CRC the target kernel checks at load time.
"""
import json
import sys

src, out, names = sys.argv[1], sys.argv[2], sys.argv[3:]
k = json.load(open(src))
with open(out, "w") as f:
    for n in names:
        e = k[n]
        f.write("0x%08x\t%s\tvmlinux\t%s\t\n" % (e["crc"], n, "EXPORT_SYMBOL_GPL" if e["gpl_only"] else "EXPORT_SYMBOL"))
print("extra.symvers: %d entries" % len(names))
