#!/usr/bin/env python3
"""Give a module built against the production kernel tree (g94) the vermagic of the debug kernel it is loaded into (gda).

usage: patch-vermagic.py <in.ko> <out.ko> <build-release> <target-release> <flags>

Why this is enough: the gda debug kernel and the g94 production kernel are built from the same source tree with the same config apart from debug options
that do not change any exported struct layout. They share module_layout and the CRC of every symbol that both export (checked with
scripts/verify/verify-ko.py), so a module that was built against g94 headers loads into gda once its "vermagic=" string says so.
The new string must be shorter than the old one; the difference is padded with NUL bytes so that no offset in the file moves.
"""
import sys

src, dst, old_rel, new_rel, flags = sys.argv[1:6]
old = ("%s %s" % (old_rel, flags)).encode()
new = ("%s %s" % (new_rel, flags)).encode()
d = bytearray(open(src, "rb").read())
key = b"vermagic=" + old + b"\0"
if d.count(key) != 1:
    sys.exit("%s: expected exactly one vermagic string for %s, found %d" % (src, old_rel, d.count(key)))
if len(new) >= len(old):
    sys.exit("target vermagic is not shorter than the build vermagic")
i = d.index(key) + len(b"vermagic=")
d[i:i + len(old)] = new + b"\0" * (len(old) - len(new))
open(dst, "wb").write(d)
print("%s: vermagic patched (%d bytes padding), %d bytes" % (dst.rsplit("/", 1)[-1], len(old) - len(new), len(d)))
