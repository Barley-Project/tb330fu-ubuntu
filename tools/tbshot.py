#!/usr/bin/env python3
"""Convert a raw framebuffer dump from /sys/kernel/tb330fu/fb0 (4864-byte stride a8r8g8b8, 1200 x 1920 visible, native portrait) into a PNG.

usage: tbshot.py <raw> <out.png> [--native] [--stride 4864] [--width 1200] [--height 1920]
By default the picture is rotated 90 degrees counter-clockwise: the camera / "top" side of the tablet is the panel's native RIGHT edge, so this is
the view a user holding the tablet in landscape sees.  --native keeps the panel's own portrait orientation.
Also prints a small content summary (mean colour, share of non-black pixels) so that a blank screen can be recognised without looking at the PNG.
"""
import sys

from PIL import Image

args = sys.argv[1:]
native = "--native" in args
args = [a for a in args if a != "--native"]


def opt(name, default):
    if name in args:
        i = args.index(name)
        v = int(args[i + 1])
        del args[i:i + 2]
        return v
    return default


stride = opt("--stride", 4864)
w = opt("--width", 1200)
h = opt("--height", 1920)
raw, out = args[0], args[1]
data = open(raw, "rb").read()
if len(data) < stride * h:
    print("short dump: %d bytes, need %d" % (len(data), stride * h))
    sys.exit(2)
im = Image.frombuffer("RGBA", (stride // 4, h), data[:stride * h], "raw", "BGRA", 0, 1).crop((0, 0, w, h)).convert("RGB")
small = im.resize((60, 96))
px = list(small.getdata())
nz = sum(1 for p in px if p != (0, 0, 0))
mean = tuple(sum(p[i] for p in px) // len(px) for i in range(3))
if not native:
    im = im.rotate(90, expand=True)
im.save(out)
print("%s %dx%d  non-black %d%%  mean RGB %s" % (out, im.size[0], im.size[1], 100 * nz // len(px), mean))
