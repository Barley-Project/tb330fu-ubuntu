#!/usr/bin/env python3
"""Read-only: extract the tb330fu_blklog snapshot from an expdb dump.

usage: 340_parse_blklog.py <expdb.img> <out-prefix> [--off 0x1120000] [--slot-size 0x6c000]

Writes <out-prefix>.F.txt (important lines from the START of the log) and <out-prefix>.T.txt (the newest part of the whole log) from the
valid slot with the larger sequence number, and prints both slot headers.  Header layout (little endian, 48 bytes):
  magic "TB330LG1", version, seq, slot, f_len, t_len, f_max, data_crc, uptime_ms, records, hcrc
crc32_le is the kernel's (init 0, no final xor), reflected polynomial 0xEDB88320.
"""
import struct
import sys

HDR = 4096
TABLE = []
for i in range(256):
    c = i
    for _ in range(8):
        c = (c >> 1) ^ (0xEDB88320 if c & 1 else 0)
    TABLE.append(c)


def crc32_le(data, crc=0):
    for b in data:
        crc = TABLE[(crc ^ b) & 0xFF] ^ (crc >> 8)
    return crc & 0xFFFFFFFF


def parse_hdr(raw):
    magic, version, seq, slot, f_len, t_len, f_max, data_crc, uptime_ms, records, hcrc = struct.unpack_from("<8s10I", raw, 0)
    ok = magic == b"TB330LG1" and version == 1 and hcrc == crc32_le(raw[:44])
    return ok, dict(magic=magic, version=version, seq=seq, slot=slot, f_len=f_len, t_len=t_len, f_max=f_max, data_crc=data_crc,
                    uptime_ms=uptime_ms, records=records)


def main():
    args = sys.argv[1:]
    off, slot_size = 0x1120000, 0x6C000
    if "--off" in args:
        i = args.index("--off")
        off = int(args[i + 1], 0)
        del args[i:i + 2]
    if "--slot-size" in args:
        i = args.index("--slot-size")
        slot_size = int(args[i + 1], 0)
        del args[i:i + 2]
    img, prefix = args[0], args[1]
    d = open(img, "rb").read()
    best = None
    for s in range(2):
        base = off + s * slot_size
        raw = d[base:base + HDR]
        ok, h = parse_hdr(raw)
        data_ok = None
        if ok:
            data = d[base + HDR:base + HDR + h["f_max"] + h["t_len"]]
            data_ok = len(data) == h["f_max"] + h["t_len"] and crc32_le(data) == h["data_crc"]
        print("slot %d @ %#x: header %s%s" % (s, base, "valid" if ok else "INVALID/empty",
                                              (", data crc %s" % ("ok" if data_ok else "BAD")) if ok else ""))
        if ok:
            print("   seq %d  uptime %.1f s  records %d  F %d bytes  T %d bytes" % (h["seq"], h["uptime_ms"] / 1000.0, h["records"], h["f_len"], h["t_len"]))
        if ok and data_ok and (best is None or h["seq"] > best[0]["seq"]):
            best = (h, data)
    if best is None:
        print("no valid snapshot found")
        sys.exit(2)
    h, data = best
    f = data[:h["f_len"]]
    t = data[h["f_max"]:h["f_max"] + h["t_len"]]
    open(prefix + ".F.txt", "wb").write(f)
    open(prefix + ".T.txt", "wb").write(t)
    print("using seq %d (uptime %.1f s): wrote %s.F.txt (%d bytes) and %s.T.txt (%d bytes)" % (h["seq"], h["uptime_ms"] / 1000.0, prefix, len(f), prefix, len(t)))


if __name__ == "__main__":
    main()
