#!/usr/bin/env python3
"""Puts the download files on Cloudflare R2, where zurbax.com links to them.

    tools/publish-r2.py PREFIX FILE...       e.g. 2.0/ build/zurbax.iso build/SHA256SUMS

Big files go up in 64 MB parts (R2 takes up to 5 GB in one piece, and the
full image is more), each part retried when the connection drops. Reads
R2_ACCOUNT_ID, R2_ACCESS_KEY_ID, R2_SECRET_ACCESS_KEY and R2_BUCKET from the
environment; nothing is printed of them.
"""
import datetime, hashlib, hmac, os, sys, time, urllib.parse, urllib.request
import xml.etree.ElementTree as ET

PART = 64 << 20
TYPES = {".iso": "application/x-iso9660-image", ".efi": "application/efi", ".sfs": "application/octet-stream"}


def env(name):
    v = os.environ.get(name)
    if not v:
        sys.exit("publish-r2: %s isn't set (add it in the environment's settings)" % name)
    return v


ACCOUNT, KEY, SECRET, BUCKET = (env(n) for n in ("R2_ACCOUNT_ID", "R2_ACCESS_KEY_ID", "R2_SECRET_ACCESS_KEY", "R2_BUCKET"))
HOST = "%s.r2.cloudflarestorage.com" % ACCOUNT


def sign(key, msg):
    return hmac.new(key, msg.encode(), hashlib.sha256).digest()


def request(method, key, query=None, body=b"", headers=None):
    """One signed S3 request (AWS signature version 4, region auto)."""
    query = query or {}
    headers = dict(headers or {})
    now = datetime.datetime.now(datetime.timezone.utc)
    amz_date, day = now.strftime("%Y%m%dT%H%M%SZ"), now.strftime("%Y%m%d")
    path = "/%s/%s" % (BUCKET, urllib.parse.quote(key, safe="/-_.~"))
    qs = "&".join("%s=%s" % (urllib.parse.quote(k, safe="-_.~"), urllib.parse.quote(str(v), safe="-_.~"))
                  for k, v in sorted(query.items()))
    headers.update({"host": HOST, "x-amz-date": amz_date, "x-amz-content-sha256": "UNSIGNED-PAYLOAD"})
    names = sorted(h.lower() for h in headers)
    canon_headers = "".join("%s:%s\n" % (n, str(headers[next(h for h in headers if h.lower() == n)]).strip()) for n in names)
    signed = ";".join(names)
    canon = "\n".join([method, path, qs, canon_headers, signed, "UNSIGNED-PAYLOAD"])
    scope = "%s/auto/s3/aws4_request" % day
    to_sign = "\n".join(["AWS4-HMAC-SHA256", amz_date, scope, hashlib.sha256(canon.encode()).hexdigest()])
    k = sign(sign(sign(sign(("AWS4" + SECRET).encode(), day), "auto"), "s3"), "aws4_request")
    sig = hmac.new(k, to_sign.encode(), hashlib.sha256).hexdigest()
    headers["Authorization"] = "AWS4-HMAC-SHA256 Credential=%s/%s, SignedHeaders=%s, Signature=%s" % (KEY, scope, signed, sig)
    del headers["host"]
    url = "https://%s%s%s" % (HOST, path, "?" + qs if qs else "")
    req = urllib.request.Request(url, data=body if method in ("PUT", "POST") else None, method=method, headers=headers)
    with urllib.request.urlopen(req, timeout=600) as r:
        return r.status, dict(r.headers), r.read()


def retried(what, fn):
    for attempt in range(5):
        try:
            return fn()
        except Exception as e:  # dropped connections, 5xx: wait and go again
            if attempt == 4:
                raise
            print("   %s failed (%s), again in %ds" % (what, e, 2 ** (attempt + 1)), flush=True)
            time.sleep(2 ** (attempt + 1))


def put(path, key):
    size = os.path.getsize(path)
    ext = os.path.splitext(path)[1]
    meta = {"Content-Type": TYPES.get(ext, "text/plain; charset=utf-8" if ext in ("", ".txt") else "application/octet-stream")}
    if ext == ".iso":
        meta["Content-Disposition"] = 'attachment; filename="%s"' % os.path.basename(path)
    if size <= PART:
        with open(path, "rb") as f:
            data = f.read()
        retried(key, lambda: request("PUT", key, body=data, headers=meta))
        print("   %s (%d bytes)" % (key, size), flush=True)
        return
    _, _, body = retried("start " + key, lambda: request("POST", key, {"uploads": ""}, headers=meta))
    upload = ET.fromstring(body).find("{*}UploadId").text
    etags, start = [], time.time()
    with open(path, "rb") as f:
        n = 0
        while True:
            chunk = f.read(PART)
            if not chunk:
                break
            n += 1
            _, h, _ = retried("part %d" % n, lambda: request("PUT", key, {"partNumber": n, "uploadId": upload}, body=chunk))
            etags.append(h.get("ETag") or h.get("etag"))
            done = f.tell()
            rate = done / max(1, time.time() - start)
            print("   %s: %d%% (%.0f MB/s)" % (key, done * 100 // size, rate / 1e6), flush=True)
    xml = "<CompleteMultipartUpload>%s</CompleteMultipartUpload>" % "".join(
        "<Part><PartNumber>%d</PartNumber><ETag>%s</ETag></Part>" % (i + 1, e) for i, e in enumerate(etags))
    retried("finish " + key, lambda: request("POST", key, {"uploadId": upload}, body=xml.encode()))
    print("   %s (%d bytes, %d parts)" % (key, size, len(etags)), flush=True)


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    prefix = sys.argv[1]
    for path in sys.argv[2:]:
        print(">> %s" % path, flush=True)
        put(path, prefix + os.path.basename(path))


main()
