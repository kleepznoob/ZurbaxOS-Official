#!/usr/bin/env python3
"""Sends commands to a headless zdisplay's control socket (development aid).

    tools/zctl.py SOCKET 'screenshot /tmp/a.png' 'click 100 200' ...
"""
import os, socket, sys, tempfile, time

def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    path = sys.argv[1]
    s = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
    me = os.path.join(tempfile.gettempdir(), "zctl-%d.sock" % os.getpid())
    s.bind(me)
    s.settimeout(10)
    try:
        for cmd in sys.argv[2:]:
            if cmd.startswith("sleep "):
                time.sleep(float(cmd.split()[1]))
                continue
            s.sendto(cmd.encode(), path)
            reply = s.recv(4096).decode().strip()
            if reply != "ok":
                print("%s -> %s" % (cmd, reply), file=sys.stderr)
    finally:
        os.unlink(me)

main()
