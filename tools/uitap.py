#!/usr/bin/env python3
"""Create a virtual multi-touch screen through /dev/uinput (same axis ranges as the Himax panel: 12000 x 19200, native portrait frame) and tap at raw positions.
usage: uitap.py x,y [x,y ...]      (raw coordinates; waits 1 s before the first tap so that the compositor picks the device up)"""
import fcntl
import os
import struct
import sys
import time

EV_SYN, EV_KEY, EV_ABS = 0, 1, 3
BTN_TOUCH = 0x14a
ABS_X, ABS_Y, ABS_MT_SLOT, ABS_MT_TRACKING_ID, ABS_MT_POSITION_X, ABS_MT_POSITION_Y = 0, 1, 0x2f, 0x39, 0x35, 0x36
INPUT_PROP_DIRECT = 1
UI_SET_EVBIT, UI_SET_KEYBIT, UI_SET_ABSBIT, UI_SET_PROPBIT = 0x40045564, 0x40045565, 0x40045567, 0x4004556e
UI_DEV_SETUP, UI_ABS_SETUP, UI_DEV_CREATE, UI_DEV_DESTROY = 0x405c5503, 0x401c5504, 0x5501, 0x5502

fd = os.open("/dev/uinput", os.O_WRONLY | os.O_NONBLOCK)
fcntl.ioctl(fd, UI_SET_EVBIT, EV_KEY)
fcntl.ioctl(fd, UI_SET_EVBIT, EV_ABS)
fcntl.ioctl(fd, UI_SET_KEYBIT, BTN_TOUCH)
fcntl.ioctl(fd, UI_SET_PROPBIT, INPUT_PROP_DIRECT)
for code, lo, hi in ((ABS_X, 0, 11999), (ABS_Y, 0, 19199), (ABS_MT_SLOT, 0, 9), (ABS_MT_TRACKING_ID, 0, 65535),
                     (ABS_MT_POSITION_X, 0, 11999), (ABS_MT_POSITION_Y, 0, 19199)):
    fcntl.ioctl(fd, UI_SET_ABSBIT, code)
    fcntl.ioctl(fd, UI_ABS_SETUP, struct.pack("<HHiiiiii", code, 0, 0, lo, hi, 0, 0, 0))
name = b"tb330fu virtual touch"
fcntl.ioctl(fd, UI_DEV_SETUP, struct.pack("<HHHH80sI", 6, int(os.environ.get("VID", "0"), 16), int(os.environ.get("PID", "0"), 16), 1, name, 0))
fcntl.ioctl(fd, UI_DEV_CREATE)


def ev(t, c, v):
    os.write(fd, struct.pack("llHHi", 0, 0, t, c, v))


time.sleep(1.5)
tid = 100
for arg in sys.argv[1:]:
    x, y = (int(v) for v in arg.split(","))
    tid += 1
    ev(EV_ABS, ABS_MT_SLOT, 0)
    ev(EV_ABS, ABS_MT_TRACKING_ID, tid)
    ev(EV_ABS, ABS_MT_POSITION_X, x)
    ev(EV_ABS, ABS_MT_POSITION_Y, y)
    ev(EV_ABS, ABS_X, x)
    ev(EV_ABS, ABS_Y, y)
    ev(EV_KEY, BTN_TOUCH, 1)
    ev(EV_SYN, 0, 0)
    time.sleep(0.15)
    ev(EV_ABS, ABS_MT_TRACKING_ID, -1)
    ev(EV_KEY, BTN_TOUCH, 0)
    ev(EV_SYN, 0, 0)
    time.sleep(1.0)
    print("tapped raw", x, y, flush=True)
fcntl.ioctl(fd, UI_DEV_DESTROY)
os.close(fd)
