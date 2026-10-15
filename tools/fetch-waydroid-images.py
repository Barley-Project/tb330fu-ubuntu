#!/usr/bin/env python3
"""Download the Waydroid Android images (LineageOS 20, arm64) on the PC, fast, and check them.

usage: fetch-waydroid-images.py [--out DIR] [--rom VANILLA|GAPPS] [--proxy http://127.0.0.1:17897] [--workers 32]

Why on the PC: the images are ~1 GB on SourceForge, and a single connection (especially through a proxy) is often only a few KB/s.  This downloads many small
byte ranges (4 MiB) over many parallel curl connections, drops connections that are slower than 150 KB/s and retries the range (on another mirror), and checks the
sha256 that the Waydroid OTA server publishes.  Needs python3 and curl.  Result: DIR/system.zip and DIR/vendor.zip; copy them to the tablet
(tools/tb.sh put DIR/*.zip /var/tmp/) and run  sudo tb330fu-waydroid-setup --images /var/tmp .
The proxy is also taken from $HTTPS_PROXY / $https_proxy when --proxy is not given.
"""
import argparse
import concurrent.futures
import hashlib
import json
import os
import queue
import subprocess
import sys
import threading
import time
import urllib.request

OTA = "https://ota.waydro.id"
MIRRORS = ["onboardcloud", "pilotfiber", "cfhcable", "versaweb", "netcologne", "ufpr", "jaist", "nchc", "freefr", "gigenet", "sitsa"]
CHUNK = 4 << 20


def fetch_json(url, proxy):
    handlers = [urllib.request.ProxyHandler({"http": proxy, "https": proxy})] if proxy else []
    with urllib.request.build_opener(*handlers).open(url, timeout=30) as r:
        return json.load(r)


def latest(url, proxy):
    entries = fetch_json(url, proxy)["response"]
    return max(entries, key=lambda e: e["datetime"])


def mirror_urls(sf_url):
    """https://sourceforge.net/projects/P/files/PATH/NAME.zip/download  ->  https://<mirror>.dl.sourceforge.net/project/P/PATH/NAME.zip"""
    rest = sf_url.split("/projects/", 1)[1]
    project, tail = rest.split("/files/", 1)
    path = tail.rsplit("/download", 1)[0]
    return ["https://%s.dl.sourceforge.net/project/%s/%s" % (m, project, path) for m in MIRRORS]


def probe(url, proxy):
    """bytes/s of a 2 MB range, 0 when the mirror does not answer"""
    cmd = ["curl", "-sL", "-r", "0-2000000", "--max-time", "12", "--connect-timeout", "6", "-o", os.devnull, "-w", "%{speed_download} %{http_code}"]
    if proxy:
        cmd += ["-x", proxy]
    try:
        out = subprocess.run(cmd + [url], capture_output=True, text=True, timeout=20).stdout.split()
        return float(out[0]) if len(out) == 2 and out[1] == "206" else 0.0
    except (subprocess.SubprocessError, ValueError, IndexError):
        return 0.0


def download(urls, size, out, sha, workers, proxy):
    n = (size + CHUNK - 1) // CHUNK
    q = queue.Queue()
    for i in range(n):
        q.put(i)
    lock = threading.Lock()
    done = [0]

    def cpath(i):
        return "%s.c%05d" % (out, i)

    def worker(wid):
        while True:
            try:
                i = q.get_nowait()
            except queue.Empty:
                return
            a = i * CHUNK
            b = min(size, a + CHUNK) - 1
            for attempt in range(200):
                have = os.path.getsize(cpath(i)) if os.path.exists(cpath(i)) else 0
                if have >= b - a + 1:
                    break
                cmd = ["curl", "-sL", "-r", "%d-%d" % (a + have, b), "--speed-limit", "150000", "--speed-time", "4", "--max-time", "120", "--connect-timeout", "8"]
                if proxy:
                    cmd += ["-x", proxy]
                with open(cpath(i), "ab") as f:
                    subprocess.run(cmd + [urls[(wid + attempt) % len(urls)]], stdout=f, stderr=subprocess.DEVNULL)
            with lock:
                done[0] += 1

    t0 = time.time()
    threads = [threading.Thread(target=worker, args=(w,), daemon=True) for w in range(workers)]
    for t in threads:
        t.start()
    last = 0
    while any(t.is_alive() for t in threads):
        time.sleep(5)
        have = sum(os.path.getsize(cpath(i)) for i in range(n) if os.path.exists(cpath(i)))
        print("  %3.0f s  %7.1f / %.1f MB  (%.1f MB/s)  %d/%d chunks" % (time.time() - t0, have / 1e6, size / 1e6, (have - last) / 5e6, done[0], n), flush=True)
        last = have
    h = hashlib.sha256()
    with open(out, "wb") as o:
        for i in range(n):
            d = open(cpath(i), "rb").read()
            if len(d) != min(size, (i + 1) * CHUNK) - i * CHUNK:
                sys.exit("chunk %d has %d bytes, a download thread died" % (i, len(d)))
            h.update(d)
            o.write(d)
    for i in range(n):
        os.remove(cpath(i))
    if h.hexdigest() != sha:
        sys.exit("SHA256 MISMATCH for %s: %s (expected %s)" % (out, h.hexdigest(), sha))
    print("  %s ok, %.0f s, sha256 %s" % (out, time.time() - t0, sha))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--out", default="waydroid-images")
    ap.add_argument("--rom", default="VANILLA", choices=["VANILLA", "GAPPS"])
    ap.add_argument("--proxy", default=os.environ.get("HTTPS_PROXY") or os.environ.get("https_proxy"))
    ap.add_argument("--workers", type=int, default=32)
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    for kind, ota in (("vendor", "%s/vendor/waydroid_arm64/MAINLINE.json" % OTA), ("system", "%s/system/lineage/waydroid_arm64/%s.json" % (OTA, a.rom))):
        e = latest(ota, a.proxy)
        print("%s: %s (%.0f MB)" % (kind, e["filename"], e["size"] / 1e6))
        urls = mirror_urls(e["url"])
        speed = sorted(((probe(u, a.proxy), u) for u in urls), reverse=True)
        best = [u for s, u in speed if s > 100000][:3]
        if not best:
            sys.exit("no SourceForge mirror answers (proxy needed? --proxy http://host:port)")
        print("  fastest mirrors:", ", ".join("%s (%.0f KB/s)" % (u.split("//")[1].split(".")[0], s / 1000) for s, u in speed[:3]))
        download(best, e["size"], os.path.join(a.out, "%s.zip" % kind), e["id"], a.workers, a.proxy)


main()
