#!/usr/bin/env python3
"""Read-only: extract the EXACT console text of the newest boot from an expdb dump using the ramoops persistent_ram_buffer
header ('DBGC', start, size) instead of timestamp heuristics.

The ramoops console zone is a ring: {u32 sig='DBGC', u32 start (write position), u32 size (valid bytes, saturates at the
zone data size), u8 data[]}.  If size < capacity the log is data[0:size]; once wrapped it is data[start:] + data[:start].
Stale text of older boots is therefore never mixed in.

usage: 183_pstore_console.py <expdb.img> [out.txt]
prints: zone offset, start/size, first/last timestamps, and the last 12 lines; writes the text to out.txt if given
"""
import re
import struct
import sys

d = open(sys.argv[1], "rb").read()
cands = []
for m in re.finditer(b"DBGC", d):
    o = m.start()
    start, size = struct.unpack_from("<II", d, o + 4)
    if size == 0 or size > 0x100000 or start > size + 0x100000:
        continue
    head = d[o + 12:o + 12 + 40]
    if re.match(rb"\[\s*\d+\.\d+\]", head) or b"Linux version" in d[o + 12:o + 12 + 200]:
        cands.append((o, start, size))
if not cands:
    print("no console zone with kernel text found")
    sys.exit(1)
o, start, size = max(cands, key=lambda c: c[2])
# capacity: distance to the next zone header is not known here; assume the standard 0x40000 console zone minus the 12 byte header
CAP = 0x40000 - 12
if size < CAP and start == size:
    text = d[o + 12:o + 12 + size]
    wrapped = False
else:
    buf = d[o + 12:o + 12 + CAP]
    text = buf[start:] + buf[:start]
    wrapped = True
text = text.replace(b"\x00", b"")
lines = text.split(b"\n")
ts = [float(m.group(1)) for l in lines for m in [re.match(rb"\[\s*(\d+\.\d+)\]", l)] if m]
print("console zone at file offset 0x%x : start=%d size=%d wrapped=%s ; %d bytes of text, %d lines" % (o, start, size, wrapped, len(text), len(lines)))
if ts:
    mono = all(ts[i] <= ts[i + 1] + 1e-9 for i in range(len(ts) - 1))
    print("timestamps: first %.6f  last %.6f  monotonic=%s" % (ts[0], ts[-1], mono))
print("---- last 12 lines")
for l in lines[-12:]:
    print("  " + l.decode("latin1")[:200])
if len(sys.argv) > 2:
    open(sys.argv[2], "wb").write(text)
    print("wrote", sys.argv[2])
