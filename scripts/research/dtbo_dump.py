#!/usr/bin/env python3
"""Read-only equivalent of `mkdtboimg.py dump <img> [-b <prefix>]`.

mkdtboimg.py is not available via pip/apt, so this re-implements only the
`dump` sub-command from the public AOSP dt_table format (system/libufdt
dt_table.h): big-endian dt_table_header (32 B) + dt_table_entry (32 B each).

  dtbo_dump.py dump <img> [-b PREFIX]   -> prints header/entries, with -b also
                                           writes each raw entry to PREFIX<N>
Works on dtbo partition images AND on the vendor_boot `dtb` section (same format).
"""
import argparse
import hashlib
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fdtlite as F  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("cmd", choices=["dump"])
    ap.add_argument("image")
    ap.add_argument("-b", "--dtb", metavar="PREFIX", default=None,
                    help="write each entry's raw blob to PREFIX<index>")
    a = ap.parse_args()

    data = open(a.image, "rb").read()
    hdr, entries = F.parse_dt_table(data)
    print("dt_table_header:")
    print("               magic = %08x" % hdr["magic"])
    for k in ("total_size", "header_size", "dt_entry_size", "dt_entry_count",
              "dt_entries_offset", "page_size", "version"):
        print("%20s = %d" % (k, hdr[k]))
    print("%20s = %d" % ("(file size)", len(data)))
    if hdr["total_size"] > len(data):
        print("!! total_size > file size")
    tail = len(data) - hdr["total_size"]
    print("%20s = %d byte(s) after total_size" % ("(trailing)", tail))
    for e in entries:
        print("dt_table_entry[%d]:" % e["index"])
        print("%20s = %d" % ("dt_size", e["dt_size"]))
        print("%20s = %d" % ("dt_offset", e["dt_offset"]))
        print("%20s = %08x" % ("id", e["id"]))
        print("%20s = %08x" % ("rev", e["rev"]))
        for i, c in enumerate(e["custom"]):
            print("%20s = %08x" % ("custom[%d]" % i, c))
        blob = data[e["dt_offset"]:e["dt_offset"] + e["dt_size"]]
        print("%20s = %s" % ("(blob)sha256", hashlib.sha256(blob).hexdigest()))
        try:
            h, root = F.parse_fdt(blob)
            print("%20s = %d (hdr totalsize=%d, version=%d)" % ("(FDT)size", len(blob), h["totalsize"], h["version"]))
            comp = root.props.get("compatible")
            if comp:
                print("%20s = %s" % ("(FDT)compatible", " ".join(F.cstrings(comp))))
            model = root.props.get("model")
            if model:
                print("%20s = %s" % ("(FDT)model", F.pstr(model)))
        except F.FdtError as ex:
            print("%20s = ERROR %s" % ("(FDT)", ex))
        if a.dtb:
            outp = "%s%d" % (a.dtb, e["index"])
            with open(outp, "wb") as f:
                f.write(blob)
            print("%20s = %s" % ("(written)", outp))


if __name__ == "__main__":
    main()
