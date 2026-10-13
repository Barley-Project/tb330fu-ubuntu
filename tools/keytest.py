#!/usr/bin/env python3
"""Listen to the hardware key input devices for N seconds (grabbed, so the desktop / logind do not react) and print every key event.
usage: keytest.py [seconds] [name-substring ...]   default: 40 s, devices named mtk-pmic-keys / mtk-kpd"""
import fcntl
import os
import select
import struct
import sys
import time

secs = int(sys.argv[1]) if len(sys.argv) > 1 else 40
want = sys.argv[2:] or ["mtk-pmic-keys", "mtk-kpd"]
EVIOCGRAB = 0x40044590
NAMES = {114: "KEY_VOLUMEDOWN", 115: "KEY_VOLUMEUP", 116: "KEY_POWER", 102: "KEY_HOME", 158: "KEY_BACK", 139: "KEY_MENU"}
fds = {}
for n in sorted(os.listdir("/sys/class/input")):
    if not n.startswith("event"):
        continue
    name = open("/sys/class/input/%s/device/name" % n).read().strip()
    if any(w in name for w in want):
        fd = os.open("/dev/input/" + n, os.O_RDONLY | os.O_NONBLOCK)
        fcntl.ioctl(fd, EVIOCGRAB, 1)
        fds[fd] = (n, name)
print("listening on %s for %d s (grabbed) - press the keys now" % (", ".join("%s=%s" % v for v in fds.values()), secs), flush=True)
t0 = time.time()
count = 0
while time.time() - t0 < secs:
    r, _, _ = select.select(list(fds), [], [], 0.5)
    for fd in r:
        try:
            data = os.read(fd, 24 * 16)
        except BlockingIOError:
            continue
        for i in range(0, len(data) - 23, 24):
            sec, usec, typ, code, val = struct.unpack("llHHi", data[i:i + 24])
            if typ == 1:
                count += 1
                print("%6.2f s  %-14s %-16s %s" % (time.time() - t0, fds[fd][1], NAMES.get(code, "code %d" % code), {0: "released", 1: "pressed", 2: "repeat"}.get(val, val)), flush=True)
for fd in fds:
    fcntl.ioctl(fd, EVIOCGRAB, 0)
print("done, %d key events" % count)
