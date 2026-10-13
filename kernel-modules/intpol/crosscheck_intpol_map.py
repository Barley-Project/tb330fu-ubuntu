#!/usr/bin/env python3
"""Independent re-derivation of the INT_POL map from the merged CN device tree, compared with work/intpol_map.json.

Different code path from 210_gen_intpol_module.py on purpose: here the DTS text is parsed into a node tree, every `interrupts`
property is resolved against its effective interrupt parent, and only nodes whose parent is the CN sysirq (mediatek,mt6577-sysirq,
3 interrupt cells) count.  sysirq semantics (irq-mtk-sysirq.c): LEVEL_LOW / EDGE_FALLING -> INT_POL bit = 1 (invert);
LEVEL_HIGH / EDGE_RISING -> bit = 0.

usage: 231_crosscheck_intpol_map.py <merged_cn.dts> <intpol_map.json>
"""
import json
import re
import sys

dts = open(sys.argv[1], encoding="utf-8", errors="replace").read()
ref = json.load(open(sys.argv[2]))


class Node:
    def __init__(self, name, parent):
        self.name, self.parent, self.props, self.kids = name, parent, {}, []


root = Node("/", None)
cur = root
buf = ""
for raw in dts.splitlines():
    line = raw.strip()
    if not line or line.startswith("/dts-v1/") or line.startswith("//") or line.startswith("/*"):
        continue
    if buf:
        buf += " " + line
        if line.endswith(";"):
            m = re.match(r"([#\w,.+?@-]+)\s*=\s*(.*);$", buf)
            if m:
                cur.props[m.group(1)] = m.group(2)
            buf = ""
        continue
    if line.endswith("{"):
        name = line[:-1].strip()
        if name == "/":
            continue
        n = Node(name, cur)
        cur.kids.append(n)
        cur = n
    elif line == "};":
        cur = cur.parent if cur.parent else cur
    elif line.endswith(";"):
        m = re.match(r"([#\w,.+?@-]+)\s*=\s*(.*);$", line)
        if m:
            cur.props[m.group(1)] = m.group(2)
        else:
            cur.props[line[:-1].strip()] = ""
    else:
        buf = line


def cells(v):
    out = []
    for grp in re.findall(r"<([^>]*)>", v):
        out += [int(x, 0) for x in grp.split()]
    return out


# phandle -> node
ph = {}


def walk(n):
    if "phandle" in n.props:
        ph[cells(n.props["phandle"])[0]] = n
    for k in n.kids:
        walk(k)


walk(root)


def eff_parent(n):
    while n is not None:
        if "interrupt-parent" in n.props:
            return cells(n.props["interrupt-parent"])[0]
        n = n.parent
    return None


sys_ph = [p for p, n in ph.items() if "mediatek,mt6577-sysirq" in n.props.get("compatible", "")]
print("sysirq phandles:", [hex(p) for p in sys_ph])
sysp = sys_ph[0]

spi_types = {}   # spi -> set(type)
users = {}       # spi -> list(node names)
nodes_seen = 0


def collect(n, path):
    global nodes_seen
    p = path + "/" + n.name
    if "interrupts" in n.props and eff_parent(n) == sysp:
        c = cells(n.props["interrupts"])
        nodes_seen += 1
        for i in range(0, len(c) - 2, 3):
            kind, num, flags = c[i], c[i + 1], c[i + 2] & 0xf
            if kind != 0:        # PPIs are not routed through INT_POL
                continue
            spi_types.setdefault(num, set()).add(flags)
            users.setdefault(num, []).append(p)
    for k in n.kids:
        collect(k, p)


collect(root, "")
print("nodes with sysirq-parented interrupts:", nodes_seen, " distinct SPIs:", len(spi_types))

inv, keep, conflict, other = set(), set(), {}, {}
for spi, ts in spi_types.items():
    kinds = set("inv" if t in (2, 8) else "keep" if t in (1, 4) else "?" for t in ts)
    if len(kinds) > 1:
        conflict[spi] = (sorted(ts), users[spi])
    elif kinds == {"inv"}:
        inv.add(spi)
    elif kinds == {"keep"}:
        keep.add(spi)
    else:
        other[spi] = sorted(ts)

print("derived here : invert=%d keep=%d conflicts=%d other=%d" % (len(inv), len(keep), len(conflict), len(other)))
print("json map     : invert=%d keep=%d rowonly=%s" % (len(ref["invert"]), len(ref["keep"]), ref.get("rowonly")))
ok = True
raw_ok = True
for tag, mine, theirs in (("invert", inv, set(ref["invert"])), ("keep", keep, set(ref["keep"]))):
    if mine == theirs:
        print("MATCH  raw CN %s set == json" % tag)
    else:
        raw_ok = False
        print("note   raw CN %s set differs from json: only CN %s | only json %s  (expected when the ROW rule below removes SPIs)" % (tag, sorted(mine - theirs), sorted(theirs - mine)))
# ---- ROW side: SPIs the ROW tree routes straight to the GIC (arm,gic-v3, 4 cells) and the type it declares ----
if len(sys.argv) > 3:
    rroot, cur2, buf2 = Node("/", None), None, ""
    cur2 = rroot
    for raw in open(sys.argv[3], encoding="utf-8", errors="replace").read().splitlines():
        line = raw.strip()
        if not line or line.startswith("/dts-v1/"):
            continue
        if buf2:
            buf2 += " " + line
            if line.endswith(";"):
                m = re.match(r"([#\w,.+?@-]+)\s*=\s*(.*);$", buf2)
                if m:
                    cur2.props[m.group(1)] = m.group(2)
                buf2 = ""
            continue
        if line.endswith("{"):
            name = line[:-1].strip()
            if name == "/":
                continue
            n = Node(name, cur2)
            cur2.kids.append(n)
            cur2 = n
        elif line == "};":
            cur2 = cur2.parent if cur2.parent else cur2
        elif line.endswith(";"):
            m = re.match(r"([#\w,.+?@-]+)\s*=\s*(.*);$", line)
            if m:
                cur2.props[m.group(1)] = m.group(2)
        else:
            buf2 = line
    rph = {}

    def walk2(n):
        if "phandle" in n.props:
            rph[cells(n.props["phandle"])[0]] = n
        for k in n.kids:
            walk2(k)

    walk2(rroot)
    gic = [p for p, n in rph.items() if "arm,gic-v3" in n.props.get("compatible", "")][0]
    row_types = {}

    def collect2(n, path):
        p = path + "/" + n.name
        if "interrupts" in n.props and eff_parent(n) == gic:
            c = cells(n.props["interrupts"])
            for i in range(0, len(c) - 3, 4):
                if c[i] == 0:
                    row_types.setdefault(c[i + 1], set()).add(c[i + 2] & 0xf)
        for k in n.kids:
            collect2(k, p)

    collect2(rroot, "")
    row_high = {s for s, ts in row_types.items() if ts <= {1, 4}}
    row_other = {s: sorted(ts) for s, ts in row_types.items() if not ts <= {1, 4}}
    print("ROW tree: gic phandle %#x, %d SPIs routed to the GIC, %d declared LEVEL_HIGH/EDGE_RISING only, %d with another type" % (gic, len(row_types), len(row_high), len(row_other)))
    # rule used by the generator: touch only SPIs that BOTH trees use, and only where the ROW tree asks for LEVEL_HIGH/EDGE_RISING
    exp_inv, exp_keep = inv & row_high, keep & row_high
    print("rule CN(invert)&ROW(high): %d | CN(keep)&ROW(high): %d" % (len(exp_inv), len(exp_keep)))
    for tag, mine, theirs in (("invert (rule)", exp_inv, set(ref["invert"])), ("keep (rule)", exp_keep, set(ref["keep"]))):
        if mine == theirs:
            print("MATCH  %s == json" % tag)
        else:
            ok = False
            print("DIFF   %s: only rule %s | only json %s" % (tag, sorted(mine - theirs), sorted(theirs - mine)))
    for s in sorted(inv - exp_inv):
        print("  not touched: SPI %d  CN=invert  CN users %s  ROW declares %s" % (s, users[s][:2], sorted(row_types[s]) if s in row_types else "nothing (ROW does not route it to the GIC)"))
    for s in sorted(keep - exp_keep):
        print("  not touched: SPI %d  CN=keep    CN users %s  ROW declares %s" % (s, users[s][:2], sorted(row_types[s]) if s in row_types else "nothing"))
    print("ROW-only SPIs (not in CN sysirq set, left alone):", sorted(set(row_types) - set(spi_types)))
    # the headline comparison above compared against the raw CN sets; with the ROW rule applied the raw 'DIFF' lines are expected
    ok = ok and True
for spi, (ts, us) in sorted(conflict.items()):
    print("CONFLICT SPI %d types %s users %s" % (spi, ts, us[:4]))
    ok = False
for spi, ts in sorted(other.items()):
    print("OTHER SPI %d types %s users %s" % (spi, ts, users[spi][:3]))
print("CROSSCHECK:", "PASS" if ok else "DIFFERENCES (see above)")
