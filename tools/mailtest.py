#!/usr/bin/env python3
"""A pretend mail server for trying Mail without a real account.

    tools/mailtest.py [DIR]

Serves IMAP and SMTP on localhost, over TLS and with STARTTLS:
    IMAP 9993 (TLS)  9143 (STARTTLS)    SMTP 9465 (TLS)  9587 (STARTTLS)
The account is test@zurbax.test with the password "secret". Mailboxes start
with sample mail (HTML with a picture inside, plain text with quotes, an
attachment, words in other alphabets). Mail sent to the account lands in its
inbox; everything sent is also written to DIR/sent/.

A certificate authority and a certificate for localhost are made in DIR on
the first run; point Mail at DIR/ca.pem with ZURBAX_CA_FILE.
"""
import base64, os, re, socket, ssl, struct, subprocess, sys, threading, time, zlib
from email import message_from_bytes
from email.utils import formatdate, parseaddr, getaddresses

USER, PASSWORD = "test@zurbax.test", "secret"
DIR = sys.argv[1] if len(sys.argv) > 1 else "/tmp/mailtest"
LOCK = threading.RLock()


def make_certs():
    os.makedirs(DIR, exist_ok=True)
    if os.path.exists(f"{DIR}/server.pem"):
        return
    run = lambda *a: subprocess.run(a, check=True, capture_output=True, cwd=DIR)
    run("openssl", "ecparam", "-name", "prime256v1", "-genkey", "-noout", "-out", "ca.key")
    run("openssl", "req", "-x509", "-new", "-key", "ca.key", "-sha256", "-days", "3650", "-subj", "/CN=Mail Test CA", "-out", "ca.pem",
        "-addext", "basicConstraints=critical,CA:TRUE", "-addext", "keyUsage=critical,keyCertSign,cRLSign")
    run("openssl", "ecparam", "-name", "prime256v1", "-genkey", "-noout", "-out", "server.key")
    run("openssl", "req", "-new", "-key", "server.key", "-subj", "/CN=localhost", "-out", "server.csr")
    with open(f"{DIR}/ext.cnf", "w") as f:
        f.write("subjectAltName=DNS:localhost,IP:127.0.0.1\nextendedKeyUsage=serverAuth\nbasicConstraints=CA:FALSE\n")
    run("openssl", "x509", "-req", "-in", "server.csr", "-CA", "ca.pem", "-CAkey", "ca.key", "-CAcreateserial", "-days", "825", "-sha256",
        "-extfile", "ext.cnf", "-out", "server.pem")


def tls_context():
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(f"{DIR}/server.pem", f"{DIR}/server.key")
    return ctx


# ---- Sample mail ----------------------------------------------------------------------------

def png(w, h):
    """A little sunset gradient, as PNG bytes."""
    rows = b""
    for y in range(h):
        rows += b"\0"
        for x in range(w):
            t = y / h
            r, g, b = int(255 - 60 * t), int(150 - 90 * t + 40 * (x / w)), int(80 + 120 * t)
            if (x - w * 0.7) ** 2 + (y - h * 0.45) ** 2 < (h * 0.18) ** 2:
                r, g, b = 255, 214, 120
            rows += bytes((r, g, b))
    chunk = lambda t, d: struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b"")


def b64lines(data):
    s = base64.b64encode(data).decode()
    return "\r\n".join(s[i:i + 76] for i in range(0, len(s), 76))


def date(days_ago, hour=10, minute=0):
    t = time.time() - days_ago * 86400
    lt = time.localtime(t)
    t = time.mktime((lt.tm_year, lt.tm_mon, lt.tm_mday, hour, minute, 0, 0, 0, -1))
    return formatdate(t, localtime=True), t


def sample_messages():
    out = []
    def add(box, raw, t, flags=()):
        out.append((box, raw.replace("\r\n", "\n").replace("\n", "\r\n").encode(), t, set(flags)))

    d, t = date(0, 9, 41)
    pic = b64lines(png(240, 120))
    add("INBOX", f"""From: Zurbax Weekly <news@zurbax.test>
To: {USER}
Subject: Your week in Zurbax: new apps, a faster Drift
Date: {d}
Message-ID: <weekly-42@zurbax.test>
MIME-Version: 1.0
Content-Type: multipart/related; boundary="REL"

--REL
Content-Type: multipart/alternative; boundary="ALT"

--ALT
Content-Type: text/plain; charset=utf-8

This week: Harbor got three new apps, and Drift lays out pages faster.
Read more at https://zurbax.test/news

--ALT
Content-Type: text/html; charset=utf-8
Content-Transfer-Encoding: quoted-printable

<!doctype html><html><head><style>
.wrap{{max-width:560px;margin:0 auto;font-family:sans-serif}}
.hero{{border-radius:12px;width:100%}}
h1{{font-size:24px;color:#1d2b53;margin:18px 0 8px}}
.btn{{display:inline-block;background:#3d7eff;color:#fff;padding:10px 18px;border-radius:8px;text-decoration:none;font-weight:bold}}
.card{{background:#f3f5f9;border-radius:10px;padding:14px 16px;margin:12px 0}}
.foot{{color:#8a8f98;font-size:12px;margin-top:24px}}
</style></head><body><div class=3D"wrap">
<img class=3D"hero" src=3D"cid:sunset@zurbax.test" width=3D"560" alt=3D"Sunset">
<h1>New this week</h1>
<p>Harbor got three new apps, and Drift now lays out big pages in half the time.</p>
<div class=3D"card"><b>Chess</b> &mdash; play the computer at four levels.</div>
<div class=3D"card"><b>Sudoku</b> &mdash; endless puzzles with notes and hints.</div>
<div class=3D"card"><b>Paint</b> &mdash; brushes, shapes and a fill bucket.</div>
<p><a class=3D"btn" href=3D"https://zurbax.test/news">Read the full story</a></p>
<img src=3D"https://tracker.example.com/open.gif" width=3D"1" height=3D"1">
<p class=3D"foot">You get this because you use Zurbax. <a href=3D"https://zurbax.test/unsubscribe">Unsubscribe</a></p>
</div></body></html>

--ALT--

--REL
Content-Type: image/png
Content-Transfer-Encoding: base64
Content-ID: <sunset@zurbax.test>
Content-Disposition: inline

{pic}
--REL--
""", t)

    d, t = date(0, 8, 15)
    add("INBOX", f"""From: =?utf-8?B?{base64.b64encode("Ioana Popescu".encode()).decode()}?= <ioana@example.ro>
To: {USER}
Subject: =?utf-8?Q?=C3=8Ent=C3=A2lnire_m=C3=A2ine_=E2=98=95?=
Date: {d}
Message-ID: <ro-1@example.ro>
MIME-Version: 1.0
Content-Type: text/plain; charset=utf-8
Content-Transfer-Encoding: quoted-printable

Bun=C4=83!

Ne vedem m=C3=A2ine la 10 la cafenea? Aduc =C8=99i schi=C8=9Bele pentru =
proiect.

Mul=C8=9Bumesc,
Ioana
""", t)

    d, t = date(1, 17, 5)
    add("INBOX", f"""From: Sam Rivera <sam@example.com>
To: {USER}, Alex Chen <alex@example.com>
Cc: Priya Natarajan <priya@example.com>
Subject: Re: Trip to the lake
Date: {d}
Message-ID: <trip-3@example.com>
In-Reply-To: <trip-2@example.com>
References: <trip-1@example.com> <trip-2@example.com>
MIME-Version: 1.0
Content-Type: text/plain; charset=us-ascii

Sounds great! I can drive. Let's leave Saturday at 8 so we catch the
sunset on the water. The cabin's listing is at www.example.com/cabin
if anyone wants to check the photos.

> Alex wrote:
> Anyone up for the lake this weekend? The weather looks perfect.
> We could rent the cabin again.

Sam
""", t, {"\\Seen"})

    d, t = date(2, 11, 30)
    sheet = "Item,Cost\nCabin,240\nGroceries,85\nFuel,40\n".encode()
    add("INBOX", f"""From: Priya Natarajan <priya@example.com>
To: {USER}
Subject: Budget for the weekend
Date: {d}
Message-ID: <budget@example.com>
MIME-Version: 1.0
Content-Type: multipart/mixed; boundary="MIX"

--MIX
Content-Type: multipart/alternative; boundary="ALT2"

--ALT2
Content-Type: text/plain; charset=utf-8

Here's the budget so far. Total comes to 365, so about 91 each.

--ALT2
Content-Type: text/html; charset=utf-8

<p>Here's the budget so far. Total comes to <b>365</b>, so about <b>91 each</b>.</p>
<table border="1" cellpadding="6" style="border-collapse:collapse"><tr><th>Item</th><th>Cost</th></tr>
<tr><td>Cabin</td><td>240</td></tr><tr><td>Groceries</td><td>85</td></tr><tr><td>Fuel</td><td>40</td></tr></table>

--ALT2--

--MIX
Content-Type: text/csv; name="budget.csv"
Content-Disposition: attachment; filename="budget.csv"
Content-Transfer-Encoding: base64

{b64lines(sheet)}
--MIX--
""", t, {"\\Seen", "\\Flagged"})

    senders = [("Harbor", "harbor@zurbax.test", "Chess 1.1 is ready to install", "A new version of Chess is available with a stronger Expert level."),
               ("Maya Lindqvist", "maya@example.se", "Photos from Saturday", "I put the photos in the shared folder, the sunset ones came out really well."),
               ("GitHub", "noreply@github.example", "[zurbax] CI passed on main", "All checks have passed for commit 4f53bea on main."),
               ("Lena Weber", "lena@example.de", "Lunch next week?", "Are you free on Tuesday or Wednesday? There's a new place near the office."),
               ("Bank Notices", "notices@bank.example", "Your statement is ready", "Your October statement is now available to view online."),
               ("Tom Okafor", "tom@example.ng", "Guitar lessons", "Thanks for the recommendation! I booked the first lesson for Thursday."),
               ("Weather Alerts", "alerts@weather.example", "Rain expected tomorrow", "Light rain is expected from 14:00 to 18:00 in your area."),
               ("Elena Rossi", "elena@example.it", "Recipe you asked for", "Here's the risotto recipe: start with a good stock and stir patiently.")]
    for i, (name, addr, subj, body) in enumerate(senders * 3):
        d, t = date(3 + i, 9 + i % 8, (i * 7) % 60)
        add("INBOX", f"""From: {name} <{addr}>
To: {USER}
Subject: {subj}
Date: {d}
Message-ID: <filler-{i}@example.com>
MIME-Version: 1.0
Content-Type: text/plain; charset=utf-8

{body}
""", t, {"\\Seen"} if i % 3 else set())

    d, t = date(1, 18, 2)
    add("Sent", f"""From: Test User <{USER}>
To: Sam Rivera <sam@example.com>
Subject: Re: Trip to the lake
Date: {d}
Message-ID: <sent-1@zurbax.test>
MIME-Version: 1.0
Content-Type: text/plain; charset=utf-8

Count me in! I'll bring the snacks.
""", t, {"\\Seen"})
    d, t = date(5, 12, 0)
    add("Junk", f"""From: Prize Department <win@lottery.example>
To: {USER}
Subject: You have WON 1,000,000!!!
Date: {d}
Message-ID: <spam-1@lottery.example>
MIME-Version: 1.0
Content-Type: text/plain; charset=utf-8

Click here to claim your prize now!
""", t)
    return out


# ---- Mailboxes ------------------------------------------------------------------------------

class Box:
    def __init__(self, name, flags):
        self.name, self.flags, self.msgs, self.next_uid = name, flags, [], 1

    def add(self, raw, t, flags):
        m = {"uid": self.next_uid, "raw": raw, "time": t, "flags": set(flags)}
        self.next_uid += 1
        self.msgs.append(m)
        return m


BOXES = {}
for name, flags in [("INBOX", ""), ("Archive", "\\Archive"), ("Drafts", "\\Drafts"), ("Sent", "\\Sent"), ("Junk", "\\Junk"),
                    ("Trash", "\\Trash"), ("Projects", ""), ("Projects/Zurbax", "")]:
    BOXES[name] = Box(name, flags)


def load_samples():
    for box, raw, t, flags in sorted(sample_messages(), key=lambda m: m[2]):
        BOXES[box].add(raw, t, flags)


def iquote(s):
    if s is None:
        return "NIL"
    if isinstance(s, bytes):
        s = s.decode("utf-8", "replace")
    if any(c in s for c in '"\\\r\n') or any(ord(c) > 127 for c in s):
        b = s.encode()
        return "{%d}\r\n" % len(b) + s
    return '"' + s + '"'


def addr_list(value):
    if not value:
        return "NIL"
    parts = []
    for name, addr in getaddresses([value]):
        box, _, host = addr.partition("@")
        parts.append("(%s NIL %s %s)" % (iquote(name) if name else "NIL", iquote(box), iquote(host)))
    return "(" + "".join(parts) + ")"


def envelope(msg):
    h = lambda k: msg.get(k)
    frm = addr_list(h("From"))
    return "(%s %s %s %s %s %s %s NIL %s %s)" % (iquote(h("Date")), iquote(h("Subject")), frm, frm, addr_list(h("Reply-To")) if h("Reply-To") else frm,
                                                 addr_list(h("To")), addr_list(h("Cc")), iquote(h("In-Reply-To")), iquote(h("Message-ID")))


def part_one(raw):
    """BODY[1]: the first part's content, as sent."""
    msg = message_from_bytes(raw)
    if not msg.is_multipart():
        return raw.split(b"\r\n\r\n", 1)[1] if b"\r\n\r\n" in raw else b""
    first = msg.get_payload(0)
    text = first.as_bytes().replace(b"\r\n", b"\n").replace(b"\n", b"\r\n")
    return text.split(b"\r\n\r\n", 1)[1] if b"\r\n\r\n" in text else b""


def literal(data):
    return b"{%d}\r\n" % len(data) + data


def internaldate(t):
    return time.strftime("%d-%b-%Y %H:%M:%S %z", time.localtime(t))


def decode_words(s):
    from email.header import decode_header
    try:
        return "".join(p.decode(c or "utf-8", "replace") if isinstance(p, bytes) else p for p, c in decode_header(s))
    except Exception:
        return s


# ---- IMAP -----------------------------------------------------------------------------------

def imap_tokens(line):
    toks, i = [], 0
    while i < len(line):
        c = line[i]
        if c == " ":
            i += 1
        elif c == '"':
            j, s = i + 1, ""
            while j < len(line) and line[j] != '"':
                if line[j] == "\\":
                    j += 1
                s += line[j]
                j += 1
            toks.append(s)
            i = j + 1
        elif c == "(":
            depth, j = 0, i
            while j < len(line):
                depth += line[j] == "("
                depth -= line[j] == ")"
                j += 1
                if depth == 0:
                    break
            toks.append(line[i:j])
            i = j
        else:
            j = i
            while j < len(line) and line[j] != " ":
                if line[j] == "[":
                    j = line.index("]", j)
                j += 1
            toks.append(line[i:j])
            i = j
    return toks


def seqset(spec, values):
    out = []
    top = max(values) if values else 0
    for piece in spec.split(","):
        a, _, b = piece.partition(":")
        a = top if a == "*" else int(a)
        b = a if not b else (top if b == "*" else int(b))
        lo, hi = min(a, b), max(a, b)
        out += [v for v in values if lo <= v <= hi]
    return out


class Imap:
    def __init__(self, sock, starttls):
        self.sock, self.starttls, self.user, self.box = sock, starttls, None, None
        self.rf = sock.makefile("rb")

    def send(self, data):
        if isinstance(data, str):
            data = data.encode()
        self.sock.sendall(data)

    def line(self, s):
        self.send(s + "\r\n")

    def caps(self):
        c = "IMAP4rev1 SASL-IR MOVE UIDPLUS SPECIAL-USE IDLE"
        return c + (" STARTTLS LOGINDISABLED" if self.starttls else " AUTH=PLAIN")

    def run(self):
        self.line("* OK [CAPABILITY %s] Mail test server ready" % self.caps())
        while True:
            raw = self.rf.readline()
            if not raw:
                return
            line = raw.decode("utf-8", "replace").rstrip("\r\n")
            tag, _, rest = line.partition(" ")
            cmd, _, args = rest.partition(" ")
            cmd = cmd.upper()
            uid = False
            if cmd == "UID":
                uid = True
                cmd, _, args = args.partition(" ")
                cmd = cmd.upper()
            try:
                if not self.dispatch(tag, cmd, args, uid):
                    return
            except Exception as e:
                self.line(f"{tag} BAD {e}")

    def need_auth(self, tag):
        if not self.user:
            self.line(f"{tag} NO not signed in")
            return True
        return False

    def dispatch(self, tag, cmd, args, uid):
        if cmd == "CAPABILITY":
            self.line("* CAPABILITY " + self.caps())
            self.line(f"{tag} OK done")
        elif cmd == "STARTTLS" and self.starttls:
            self.line(f"{tag} OK begin TLS")
            self.sock = tls_context().wrap_socket(self.sock, server_side=True)
            self.rf = self.sock.makefile("rb")
            self.starttls = False
        elif cmd == "NOOP":
            self.line(f"{tag} OK done")
        elif cmd == "LOGOUT":
            self.line("* BYE")
            self.line(f"{tag} OK bye")
            return False
        elif cmd == "LOGIN":
            u, p = imap_tokens(args)[:2]
            self.auth(tag, u, p)
        elif cmd == "AUTHENTICATE":
            mech, _, ir = args.partition(" ")
            if not ir:
                self.line("+ ")
                ir = self.rf.readline().decode().strip()
            _, u, p = base64.b64decode(ir).decode().split("\0")
            self.auth(tag, u, p)
        elif self.need_auth(tag):
            pass
        elif cmd == "LIST":
            for b in BOXES.values():
                flags = "\\HasChildren" if any(o.startswith(b.name + "/") for o in BOXES) else "\\HasNoChildren"
                if b.flags:
                    flags += " " + b.flags
                self.line('* LIST (%s) "/" %s' % (flags, iquote(b.name)))
            self.line(f"{tag} OK done")
        elif cmd in ("SELECT", "EXAMINE"):
            name = imap_tokens(args)[0]
            if name.upper() == "INBOX":
                name = "INBOX"
            if name not in BOXES:
                self.line(f"{tag} NO no such mailbox")
                return True
            self.box = BOXES[name]
            with LOCK:
                self.line(f"* {len(self.box.msgs)} EXISTS")
                self.line("* 0 RECENT")
                self.line("* OK [UIDVALIDITY 7] ok")
                self.line(f"* OK [UIDNEXT {self.box.next_uid}] ok")
                self.line("* FLAGS (\\Seen \\Flagged \\Answered \\Deleted \\Draft)")
            self.line(f"{tag} OK [READ-WRITE] selected")
        elif cmd == "STATUS":
            toks = imap_tokens(args)
            name = "INBOX" if toks[0].upper() == "INBOX" else toks[0]
            b = BOXES.get(name)
            if not b:
                self.line(f"{tag} NO no such mailbox")
                return True
            with LOCK:
                unseen = sum(1 for m in b.msgs if "\\Seen" not in m["flags"])
                self.line("* STATUS %s (MESSAGES %d UNSEEN %d)" % (iquote(b.name), len(b.msgs), unseen))
            self.line(f"{tag} OK done")
        elif cmd == "FETCH":
            self.fetch(tag, args, uid)
        elif cmd == "STORE":
            spec, op, flags = args.split(" ", 2)
            flags = set(flags.strip("()").split())
            with LOCK:
                for m in self.pick(spec, uid):
                    if op.startswith("+"):
                        m["flags"] |= flags
                    elif op.startswith("-"):
                        m["flags"] -= flags
                    else:
                        m["flags"] = set(flags)
            self.line(f"{tag} OK done")
        elif cmd in ("MOVE", "COPY"):
            spec, dest = args.split(" ", 1)
            dest = imap_tokens(dest)[0]
            if dest.upper() == "INBOX":
                dest = "INBOX"
            if dest not in BOXES:
                self.line(f"{tag} NO [TRYCREATE] no such mailbox")
                return True
            with LOCK:
                for m in self.pick(spec, uid):
                    BOXES[dest].add(m["raw"], m["time"], m["flags"] - {"\\Deleted"})
                    if cmd == "MOVE":
                        seq = self.box.msgs.index(m) + 1
                        self.box.msgs.remove(m)
                        self.line(f"* {seq} EXPUNGE")
            self.line(f"{tag} OK done")
        elif cmd == "EXPUNGE":
            with LOCK:
                keep = set(m["uid"] for m in self.pick(args, True)) if uid and args else None
                for m in list(self.box.msgs):
                    if "\\Deleted" in m["flags"] and (keep is None or m["uid"] in keep):
                        self.line(f"* {self.box.msgs.index(m) + 1} EXPUNGE")
                        self.box.msgs.remove(m)
            self.line(f"{tag} OK done")
        elif cmd == "SEARCH":
            words = re.findall(r'TEXT "((?:[^"\\]|\\.)*)"', args) + re.findall(r'TEXT ([^" ]\S*)', args)
            found = []
            with LOCK:
                for i, m in enumerate(self.box.msgs):
                    text = m["raw"].decode("utf-8", "replace").lower()
                    msg = message_from_bytes(m["raw"])
                    text += " " + decode_words(msg.get("Subject", "")).lower() + " " + decode_words(msg.get("From", "")).lower()
                    if all(w.lower() in text for w in words):
                        found.append(m["uid"] if uid else i + 1)
            self.line("* SEARCH" + "".join(f" {v}" for v in found))
            self.line(f"{tag} OK done")
        elif cmd == "APPEND":
            mlit = re.search(r"\{(\d+)\}$", args)
            toks = imap_tokens(args[:mlit.start()])
            name = "INBOX" if toks[0].upper() == "INBOX" else toks[0]
            flags = set(toks[1].strip("()").split()) if len(toks) > 1 and toks[1].startswith("(") else set()
            self.line("+ Ready")
            data = self.rf.read(int(mlit.group(1)))
            self.rf.readline()
            if name not in BOXES:
                self.line(f"{tag} NO [TRYCREATE] no such mailbox")
                return True
            with LOCK:
                BOXES[name].add(data, time.time(), flags)
            self.line(f"{tag} OK appended")
        else:
            self.line(f"{tag} BAD unknown command {cmd}")
        return True

    def auth(self, tag, u, p):
        if u == USER and p == PASSWORD:
            self.user = u
            self.line(f"{tag} OK [CAPABILITY {self.caps()}] signed in")
        else:
            self.line(f"{tag} NO [AUTHENTICATIONFAILED] Invalid credentials")

    def pick(self, spec, uid):
        msgs = self.box.msgs
        if uid:
            want = set(seqset(spec, [m["uid"] for m in msgs]))
            return [m for m in msgs if m["uid"] in want]
        want = set(seqset(spec, list(range(1, len(msgs) + 1))))
        return [m for i, m in enumerate(msgs) if i + 1 in want]

    def fetch(self, tag, args, uid):
        spec, items = args.split(" ", 1)
        items = items.upper()
        with LOCK:
            for m in self.pick(spec, uid):
                seq = self.box.msgs.index(m) + 1
                out = [b"UID %d" % m["uid"]]
                msg = message_from_bytes(m["raw"])
                if "FLAGS" in items:
                    out.append(("FLAGS (%s)" % " ".join(sorted(m["flags"]))).encode())
                if "INTERNALDATE" in items:
                    out.append(('INTERNALDATE "%s"' % internaldate(m["time"])).encode())
                if "RFC822.SIZE" in items:
                    out.append(b"RFC822.SIZE %d" % len(m["raw"]))
                if "ENVELOPE" in items:
                    out.append(b"ENVELOPE " + envelope(msg).encode())
                if "BODY.PEEK[1]" in items or "BODY[1]" in items:
                    part = part_one(m["raw"])
                    r = re.search(r"BODY(?:\.PEEK)?\[1\]<(\d+)\.(\d+)>", items)
                    if r:
                        a, n = int(r.group(1)), int(r.group(2))
                        out.append(b"BODY[1]<%d> " % a + literal(part[a:a + n]))
                    else:
                        out.append(b"BODY[1] " + literal(part))
                if "BODY.PEEK[]" in items or "BODY[]" in items or "RFC822" == items.strip("()"):
                    out.append(b"BODY[] " + literal(m["raw"]))
                    if "BODY[]" in items and "PEEK" not in items:
                        m["flags"].add("\\Seen")
                self.send(b"* %d FETCH (" % seq + b" ".join(out) + b")\r\n")
        self.line(f"{tag} OK done")


# ---- SMTP -----------------------------------------------------------------------------------

def smtp(sock, starttls):
    rf = sock.makefile("rb")
    send = lambda s: sock.sendall((s + "\r\n").encode())
    send("220 localhost Mail test server")
    authed, rcpt, sender = False, [], None
    while True:
        raw = rf.readline()
        if not raw:
            return
        line = raw.decode("utf-8", "replace").rstrip("\r\n")
        verb = line.split(" ", 1)[0].upper()
        if verb in ("EHLO", "HELO"):
            send("250-localhost")
            send("250-SIZE 35882577")
            send("250-8BITMIME")
            send("250-STARTTLS" if starttls else "250-AUTH PLAIN LOGIN")
            send("250 SMTPUTF8")
        elif verb == "STARTTLS" and starttls:
            send("220 go ahead")
            sock = tls_context().wrap_socket(sock, server_side=True)
            rf = sock.makefile("rb")
            send = lambda s, sock=sock: sock.sendall((s + "\r\n").encode())
            starttls = False
        elif verb == "AUTH":
            parts = line.split(" ")
            if parts[1].upper() == "PLAIN":
                ir = parts[2] if len(parts) > 2 else None
                if not ir:
                    send("334 ")
                    ir = rf.readline().decode().strip()
                _, u, p = base64.b64decode(ir).decode().split("\0")
            else:
                send("334 VXNlcm5hbWU6")
                u = base64.b64decode(rf.readline().strip()).decode()
                send("334 UGFzc3dvcmQ6")
                p = base64.b64decode(rf.readline().strip()).decode()
            authed = u == USER and p == PASSWORD
            send("235 2.7.0 accepted" if authed else "535 5.7.8 Username and Password not accepted")
        elif verb == "MAIL":
            if not authed:
                send("530 5.7.0 Authentication required")
                continue
            sender, rcpt = line[10:].strip(), []
            send("250 OK")
        elif verb == "RCPT":
            rcpt.append(line[8:].strip().strip("<>"))
            send("250 OK")
        elif verb == "DATA":
            send("354 go ahead")
            data = b""
            while True:
                l = rf.readline()
                if l in (b".\r\n", b"."):
                    break
                if l.startswith(b".."):
                    l = l[1:]
                data += l
            os.makedirs(f"{DIR}/sent", exist_ok=True)
            name = f"{DIR}/sent/{int(time.time() * 1000)}.eml"
            with open(name, "wb") as f:
                f.write(data)
            print(f"smtp: from {sender} to {', '.join(rcpt)} -> {name}", flush=True)
            if USER in rcpt:
                with LOCK:
                    BOXES["INBOX"].add(data, time.time(), set())
            send("250 2.0.0 queued")
        elif verb == "RSET":
            rcpt = []
            send("250 OK")
        elif verb == "NOOP":
            send("250 OK")
        elif verb == "QUIT":
            send("221 bye")
            return
        else:
            send("502 unknown command")


# ---- Listening ------------------------------------------------------------------------------

def serve(port, handler, tls, starttls):
    ls = socket.socket()
    ls.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    ls.bind(("127.0.0.1", port))
    ls.listen(16)

    def one(conn):
        try:
            if tls:
                conn = tls_context().wrap_socket(conn, server_side=True)
            handler(conn, starttls)
        except Exception as e:
            print(f"port {port}: {e}", flush=True)
        finally:
            try:
                conn.close()
            except Exception:
                pass

    while True:
        conn, _ = ls.accept()
        threading.Thread(target=one, args=(conn,), daemon=True).start()


def main():
    make_certs()
    load_samples()
    for port, handler, tls, starttls in [(9993, lambda s, st: Imap(s, st).run(), True, False), (9143, lambda s, st: Imap(s, st).run(), False, True),
                                         (9465, smtp, True, False), (9587, smtp, False, True)]:
        threading.Thread(target=serve, args=(port, handler, tls, starttls), daemon=True).start()
    print(f"mail test server: {USER} / {PASSWORD}; CA at {DIR}/ca.pem", flush=True)
    while True:
        time.sleep(3600)


if __name__ == "__main__":
    main()
