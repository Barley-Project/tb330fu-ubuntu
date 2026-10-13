#!/usr/bin/env python3
"""Recover build and verification data from a vendor kernel image, offline.

usage: extract_kernel_data.py <boot.img> <out dir>

Reads an Android boot image (header v3/v4), decompresses the kernel and writes into <out dir>:
  Image            the raw arm64 kernel
  config           the kernel configuration (IKCONFIG, i.e. /proc/config.gz of that kernel)
  System.map       symbol addresses recovered from the embedded base-relative kallsyms tables
  exports.json     {symbol: {crc, address, namespace, gpl_only}} recovered from __ksymtab/__kcrctab (+ _gpl)
  Module.symvers   the same export table in Kbuild format: the CRCs the vendor kernel really uses (the vendor build only ships the finished image)

Nothing here is guessed: every table is located by structure and checked for consistency (monotonic addresses, matching counts); the script stops otherwise.
Used for: preparing the Kbuild tree (Module.symvers, config), and for verifying that a module's imports exist in the target kernel with the right CRC.
"""
import gzip
import json
import os
import re
import struct
import subprocess
import sys


def u32(b, o):
    return struct.unpack_from("<I", b, o)[0]


def u64(b, o):
    return struct.unpack_from("<Q", b, o)[0]


def align(n, a):
    return (n + a - 1) // a * a


def cstr(b):
    return b.split(b"\0")[0].decode("utf-8", "replace")


def decompress(b):
    if b[:2] == b"\x1f\x8b":
        return gzip.decompress(b)
    if b[:4] in (bytes.fromhex("02214c18"), bytes.fromhex("04224d18")):
        p = subprocess.run(["lz4", "-d", "-c"], input=b, capture_output=True)
        if p.returncode:
            sys.exit("lz4: %s" % p.stderr.decode())
        return p.stdout
    return b


def kernel_from_boot_img(b):
    if b[:8] != b"ANDROID!":
        sys.exit("not an Android boot image")
    ver = u32(b, 40)
    if ver not in (3, 4):
        sys.exit("unsupported boot header version %d" % ver)
    ksize = u32(b, 8)
    return decompress(b[4096:4096 + ksize])


def ikconfig(img):
    a = img.find(b"IKCFG_ST")
    if a < 0:
        return None
    a += 8
    z = img.find(b"IKCFG_ED", a)
    return gzip.decompress(img[a:z]).decode()


def recover(b):
    """locate the embedded kallsyms tables (Linux 6.6 layout: names, markers, token table, token index, offsets, relative base)"""
    candidates = []
    for m in re.finditer(rb"\x00\x00(?:[\x01-\xff]\x00){3}", b):
        pos = m.start()
        if pos % 2 or pos + 512 > len(b):
            continue
        idx = struct.unpack_from("<256H", b, pos)
        if not all(0 < z - a < 80 for a, z in zip(idx, idx[1:])):
            continue
        for delta in range(2, 88):
            start = pos - idx[-1] - delta
            if start < 0:
                continue
            tokens = []
            valid = True
            for i, o in enumerate(idx):
                e = b.find(b"\0", start + o, min(pos, start + o + 80))
                if e < 0 or (i < 255 and e + 1 != start + idx[i + 1]):
                    valid = False
                    break
                token = b[start + o:e]
                if any(x < 32 or x >= 127 for x in token):
                    valid = False
                    break
                tokens.append(token.decode())
            if valid and not any(b[e + 1:pos]):
                candidates.append((start, pos, tokens))
                break
    for start, ipos, tokens in candidates:
        low = max(0, start - 6 * 1024 * 1024)
        for pos in range(align(low, 8), start, 8):
            n = u32(b, pos)
            if not 10000 < n < 500000 or u32(b, pos + 4) != 0:
                continue
            offpos = align(ipos + 512, 8)
            if offpos + 4 * n + 16 > len(b):
                continue
            relbase = u64(b, align(offpos + 4 * n, 8))
            cur = pos + 8
            names = []
            markers = []
            ok = True
            try:
                for i in range(n):
                    if i % 256 == 0:
                        markers.append(cur - (pos + 8))
                    sz = b[cur]
                    cur += 1
                    if sz & 0x80:
                        sz = (sz & 0x7f) | (b[cur] << 7)
                        cur += 1
                    if not 0 < sz < 1024 or cur + sz >= start:
                        ok = False
                        break
                    s = "".join(tokens[t] for t in b[cur:cur + sz])
                    cur += sz
                    if not s or s[0] not in "TtDdRrBbSsAaWwVvNnUu" or any(ord(x) < 32 for x in s):
                        ok = False
                        break
                    names.append(s)
            except (IndexError, ValueError):
                ok = False
            if not ok or len(names) != n:
                continue
            markpos = align(cur, 8)
            markend = markpos + len(markers) * 4
            if align(markend, 8) != start or tuple(markers) != struct.unpack_from("<" + str(len(markers)) + "I", b, markpos):
                continue
            offsets = struct.unpack_from("<" + str(n) + "I", b, offpos)
            addresses = [(relbase + x) & 0xffffffffffffffff for x in offsets]
            if not all(a <= z for a, z in zip(addresses, addresses[1:])):
                continue
            return list(zip(addresses, names))
    sys.exit("no self-consistent base-relative kallsyms found")


def kernel_exports(image, symbols):
    sym = {}
    for a, n in symbols:
        sym.setdefault(n[1:], a)
    base = sym["_text"]
    out = {}
    for suffix in ("", "_gpl"):
        start = sym["__start___ksymtab" + suffix] - base
        end = sym["__stop___ksymtab" + suffix] - base
        cstart = sym["__start___kcrctab" + suffix] - base
        cend = sym["__stop___kcrctab" + suffix] - base
        assert (end - start) % 12 == 0 and (end - start) // 12 == (cend - cstart) // 4
        for i, p in enumerate(range(start, end, 12)):
            value, no, ns = struct.unpack_from("<3i", image, p)
            namepos = p + 4 + no
            assert 0 <= namepos < len(image)
            name = cstr(image[namepos:namepos + 256])
            assert name and all(c.isalnum() or c == "_" for c in name)
            nspos = p + 8 + ns
            assert 0 <= nspos < len(image)
            out[name] = {"crc": u32(image, cstart + 4 * i), "address": hex(base + p + value), "namespace": cstr(image[nspos:nspos + 256]),
                         "gpl_only": suffix == "_gpl"}
    return out


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    src, out = sys.argv[1:3]
    os.makedirs(out, exist_ok=True)
    img = kernel_from_boot_img(open(src, "rb").read())
    open(os.path.join(out, "Image"), "wb").write(img)
    cfg = ikconfig(img)
    if cfg:
        open(os.path.join(out, "config"), "w").write(cfg)
    symbols = recover(img)
    open(os.path.join(out, "System.map"), "w").write("".join("%016x %s %s\n" % (a, n[0], n[1:]) for a, n in symbols))
    exports = kernel_exports(img, symbols)
    json.dump(exports, open(os.path.join(out, "exports.json"), "w"), indent=1)
    with open(os.path.join(out, "Module.symvers"), "w") as f:
        for n, x in sorted(exports.items()):
            f.write("0x%08x\t%s\tvmlinux\t%s\t%s\n" % (x["crc"], n, "EXPORT_SYMBOL_GPL" if x["gpl_only"] else "EXPORT_SYMBOL", x["namespace"]))
    print("%s: kernel %d bytes, %d kallsyms, %d exported symbols, config %s" % (src, len(img), len(symbols), len(exports), "found" if cfg else "NOT found"))


if __name__ == "__main__":
    main()
