#!/usr/bin/env python3
"""Puts the download files on the Internet Archive, where zurbax.com links to them.

    tools/publish-ia.py ITEM FILE...       e.g. zurbax-2.0 build/zurbax.iso build/SHA256SUMS

Uses the Archive's S3-like API: the item is made with the first file, each
file is sent whole (streamed from disk, so the full image needs no memory)
and sent again from the start if the connection drops. The files are then at
https://archive.org/download/ITEM/NAME. Reads IA_ACCESS_KEY and IA_SECRET_KEY
from the environment; nothing is printed of them.
"""
import http.client, os, ssl, sys, time, urllib.parse

HOST = "s3.us.archive.org"


def env(name):
    v = os.environ.get(name)
    if not v:
        sys.exit("publish-ia: %s isn't set (add it in the environment's settings)" % name)
    return v


AUTH = "LOW %s:%s" % (env("IA_ACCESS_KEY"), env("IA_SECRET_KEY"))


def connection():
    """Through the HTTPS proxy (a CONNECT tunnel) when there is one."""
    ctx = ssl.create_default_context()
    proxy = os.environ.get("HTTPS_PROXY") or os.environ.get("https_proxy")
    if not proxy:
        return http.client.HTTPSConnection(HOST, 443, context=ctx, timeout=600)
    p = urllib.parse.urlparse(proxy)
    c = http.client.HTTPSConnection(p.hostname, p.port or 80, context=ctx, timeout=600)
    c.set_tunnel(HOST, 443)
    return c


def put(item, path, first):
    size = os.path.getsize(path)
    name = os.path.basename(path)
    headers = {"authorization": AUTH, "Content-Length": str(size), "x-archive-size-hint": str(size),
               "x-archive-queue-derive": "0", "x-archive-interactive-priority": "1"}
    if first:
        headers.update({
            "x-archive-auto-make-bucket": "1",
            "x-archive-meta-mediatype": "software",
            "x-archive-meta-collection": "opensource_media",
            "x-archive-meta-title": "Zurbax 2.0",
            "x-archive-meta-description": "Zurbax 2.0, a desktop operating system made from scratch on the Linux kernel: "
                                          "Steam with Proton, Flathub and drivers for every graphics card. zurbax.com",
            "x-archive-meta-subject": "operating system;linux;gaming",
            "x-archive-meta-licenseurl": "https://zurbax.com",
        })
    for attempt in range(4):
        c = connection()
        try:
            c.putrequest("PUT", "/%s/%s" % (item, urllib.parse.quote(name)), skip_accept_encoding=True)
            for k, v in headers.items():
                c.putheader(k, v)
            c.endheaders()
            sent, start, last = 0, time.time(), 0
            with open(path, "rb") as f:
                while True:
                    chunk = f.read(1 << 20)
                    if not chunk:
                        break
                    c.send(chunk)
                    sent += len(chunk)
                    if time.time() - last > 15:
                        last = time.time()
                        print("   %s: %d%% (%.0f MB/s)" % (name, sent * 100 // max(1, size), sent / max(1, last - start) / 1e6), flush=True)
            r = c.getresponse()
            body = r.read().decode(errors="replace")
            if r.status in (200, 201):
                print("   %s (%d bytes): https://archive.org/download/%s/%s" % (name, size, item, name), flush=True)
                return
            raise RuntimeError("HTTP %d %s" % (r.status, body[:200]))
        except Exception as e:
            if attempt == 3:
                raise
            print("   %s failed (%s), sending again in %ds" % (name, e, 10 * (attempt + 1)), flush=True)
            time.sleep(10 * (attempt + 1))
        finally:
            c.close()


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    item = sys.argv[1]
    for i, path in enumerate(sys.argv[2:]):
        print(">> %s" % path, flush=True)
        put(item, path, i == 0)


main()
