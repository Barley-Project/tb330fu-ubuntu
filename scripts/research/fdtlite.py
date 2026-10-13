#!/usr/bin/env python3
"""Tiny read-only FDT / Android DT-table parser used by the analysis scripts.

No third-party dependencies.  Never writes anything except where a caller asks.
"""
import struct

FDT_MAGIC = 0xD00DFEED
DT_TABLE_MAGIC = 0xD7B7AB1E
FDT_BEGIN_NODE, FDT_END_NODE, FDT_PROP, FDT_NOP, FDT_END = 1, 2, 3, 4, 9


class FdtError(Exception):
    pass


class Node:
    __slots__ = ("name", "props", "children", "parent")

    def __init__(self, name, parent=None):
        self.name = name
        self.props = {}          # name -> bytes (insertion ordered)
        self.children = []       # list[Node]
        self.parent = parent

    def path(self):
        if self.parent is None:
            return "/"
        parts = []
        n = self
        while n.parent is not None:
            parts.append(n.name)
            n = n.parent
        return "/" + "/".join(reversed(parts))

    def child(self, name):
        for c in self.children:
            if c.name == name:
                return c
        return None

    def walk(self):
        yield self
        for c in self.children:
            yield from c.walk()


def fdt_header(buf, off=0):
    if len(buf) - off < 40:
        raise FdtError("short buffer")
    (magic, totalsize, off_struct, off_strings, off_memrsv, version,
     last_comp, boot_cpu, size_strings, size_struct) = struct.unpack_from(">10I", buf, off)
    if magic != FDT_MAGIC:
        raise FdtError("bad FDT magic 0x%08x at %d" % (magic, off))
    return dict(magic=magic, totalsize=totalsize, off_dt_struct=off_struct,
                off_dt_strings=off_strings, off_mem_rsvmap=off_memrsv,
                version=version, last_comp_version=last_comp,
                boot_cpuid_phys=boot_cpu, size_dt_strings=size_strings,
                size_dt_struct=size_struct)


def parse_fdt(buf, off=0):
    """Return (header, root Node).  `buf` may be longer than the FDT."""
    h = fdt_header(buf, off)
    if off + h["totalsize"] > len(buf):
        raise FdtError("totalsize %d runs past buffer (%d)" % (h["totalsize"], len(buf) - off))
    st = off + h["off_dt_struct"]
    ss = off + h["off_dt_strings"]
    end = st + h["size_dt_struct"]
    p = st
    root = None
    cur = None
    while p < end:
        (tok,) = struct.unpack_from(">I", buf, p)
        p += 4
        if tok == FDT_BEGIN_NODE:
            z = buf.index(b"\0", p)
            name = buf[p:z].decode("utf-8", "replace")
            p = (z + 1 + 3) & ~3
            n = Node(name, cur)
            if cur is None:
                root = n
            else:
                cur.children.append(n)
            cur = n
        elif tok == FDT_END_NODE:
            cur = cur.parent
        elif tok == FDT_PROP:
            ln, nameoff = struct.unpack_from(">II", buf, p)
            p += 8
            z = buf.index(b"\0", ss + nameoff)
            pname = buf[ss + nameoff:z].decode("utf-8", "replace")
            cur.props[pname] = bytes(buf[p:p + ln])
            p = (p + ln + 3) & ~3
        elif tok == FDT_NOP:
            continue
        elif tok == FDT_END:
            break
        else:
            raise FdtError("bad token 0x%x at %d" % (tok, p - 4))
    return h, root


def lookup(root, path):
    n = root
    for part in [x for x in path.split("/") if x]:
        n = n.child(part)
        if n is None:
            return None
    return n


def cstrings(b):
    """Split a property value into NUL-terminated strings."""
    return [s.decode("utf-8", "replace") for s in b.split(b"\0") if s != b""] if b else []


def pstr(b):
    return b.split(b"\0", 1)[0].decode("utf-8", "replace")


# ---------------------------------------------------------------- DT table
def parse_dt_table(data):
    """Parse an Android DT table (dtbo / vendor_boot dtb section).  Big-endian."""
    if len(data) < 32:
        raise FdtError("too short for dt_table_header")
    (magic, total_size, header_size, entry_size, entry_count, entries_off,
     page_size, version) = struct.unpack_from(">8I", data, 0)
    if magic != DT_TABLE_MAGIC:
        raise FdtError("not a DT table (magic 0x%08x)" % magic)
    hdr = dict(magic=magic, total_size=total_size, header_size=header_size,
               dt_entry_size=entry_size, dt_entry_count=entry_count,
               dt_entries_offset=entries_off, page_size=page_size, version=version)
    entries = []
    for i in range(entry_count):
        o = entries_off + i * entry_size
        dt_size, dt_off, eid, rev = struct.unpack_from(">4I", data, o)
        custom = struct.unpack_from(">4I", data, o + 16)
        entries.append(dict(index=i, dt_size=dt_size, dt_offset=dt_off, id=eid,
                            rev=rev, custom=custom))
    return hdr, entries
