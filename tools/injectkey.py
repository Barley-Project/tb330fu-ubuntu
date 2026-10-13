#!/usr/bin/env python3
"""Inject key presses through uinput (a virtual keyboard) to see what the desktop does with them. usage: injectkey.py <keycode> [hold-seconds]"""
import fcntl
import os
import struct
import sys
import time

code = int(sys.argv[1])
hold = float(sys.argv[2]) if len(sys.argv) > 2 else 0.15
UI_SET_EVBIT, UI_SET_KEYBIT, UI_DEV_CREATE, UI_DEV_DESTROY = 0x40045564, 0x40045565, 0x5501, 0x5502
fd = os.open("/dev/uinput", os.O_WRONLY | os.O_NONBLOCK)
fcntl.ioctl(fd, UI_SET_EVBIT, 1)
fcntl.ioctl(fd, UI_SET_EVBIT, 0)
for k in (code, 1, 114, 115, 116):
    fcntl.ioctl(fd, UI_SET_KEYBIT, k)
os.write(fd, struct.pack("80sHHHHi64i64i64i64i", b"tb330fu test keys", 0x03, 0x1d6b, 0x0331, 1, 0, *([0] * 256)))
fcntl.ioctl(fd, UI_DEV_CREATE)
time.sleep(1.0)


def ev(t, c, v):
    os.write(fd, struct.pack("llHHi", 0, 0, t, c, v))


ev(1, code, 1); ev(0, 0, 0)
time.sleep(hold)
ev(1, code, 0); ev(0, 0, 0)
time.sleep(0.5)
fcntl.ioctl(fd, UI_DEV_DESTROY)
os.close(fd)
print("injected key %d" % code)
