#!/usr/bin/env python3
"""Talk to the tablet's CDC-ACM serial console (the USB gadget's second function; a login prompt runs on ttyGS0).

usage:
  tbserial.py list                       list COM ports with their USB ids
  tbserial.py run "<command>" [port]     log in as $TB_USER (password $TB_PASSWORD), run one command, print its output
  tbserial.py raw [seconds] [port]       just print whatever the port sends for a few seconds
The port is auto-detected: the one whose description / hwid mentions VID 1D6B PID 0105 (the gadget).
"""
import os
import sys
import time

import serial
import serial.tools.list_ports as lp


def find():
    for p in lp.comports():
        if "VID:PID=1D6B:0105" in (p.hwid or "").upper():
            return p.device
    return None


def main():
    a = sys.argv[1:]
    if not a or a[0] == "list":
        for p in lp.comports():
            print(p.device, p.description, p.hwid)
        return 0
    if a[0] == "raw":
        secs = float(a[1]) if len(a) > 1 else 5
        port = a[2] if len(a) > 2 else find()
        s = serial.Serial(port, 115200, timeout=0.2)
        t = time.time()
        while time.time() - t < secs:
            d = s.read(4096)
            if d:
                sys.stdout.write(d.decode("utf-8", "replace"))
                sys.stdout.flush()
        return 0
    if a[0] == "run":
        cmd = a[1]
        port = a[2] if len(a) > 2 else find()
        if not port:
            print("no gadget serial port found")
            return 2
        s = serial.Serial(port, 115200, timeout=0.3)

        def rd(t):
            end = time.time() + t
            buf = b""
            while time.time() < end:
                d = s.read(4096)
                if d:
                    buf += d
                    end = max(end, time.time() + 0.6)
            return buf.decode("utf-8", "replace")

        s.write(b"\r\n")
        o = rd(2)
        if "login:" in o:
            s.write(b"ubuntu\r\n")
            o = rd(2)
            if "assword" in o:
                s.write(b"ubuntu\r\n")
                o = rd(3)
        marker = "TBDONE%d" % int(time.time())
        s.write(("%s; echo %s\r\n" % (cmd, marker)).encode())
        out = ""
        t0 = time.time()
        while marker + "\r" not in out.replace(cmd, "") and time.time() - t0 < 60:
            out += rd(2)
        sys.stdout.write(out)
        return 0
    return 1


sys.exit(main())
