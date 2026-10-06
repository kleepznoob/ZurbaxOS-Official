#!/usr/bin/env python3
"""Drives Zurbax in QEMU for end-to-end tests: boots with UEFI firmware, takes
screenshots, clicks and types through QMP.

    tools/qemu.py OUTDIR [--stick IMG] [--disk IMG] [--vars FILE] [--debug] SCRIPT...

SCRIPT steps:
    wait N              sleep N seconds
    shot NAME           save OUTDIR/NAME.png
    click X Y           left click at screen pixel X, Y
    type TEXT           type ASCII text
    key NAME            press a key (QEMU qcode: ret, tab, esc...)
    waitoff N           wait up to N seconds for the machine to power off or reboot

--stick attaches an image as a removable USB stick; --disk as an internal
virtio disk. --vars keeps the firmware variables (boot entries) in FILE.
--audio-wav FILE adds an Intel HDA sound card whose output is recorded to FILE.
"""
import json, os, shutil, socket, subprocess, sys, time

W, H = 1280, 800


class Qmp:
    def __init__(self, path):
        for _ in range(100):
            try:
                self.s = socket.socket(socket.AF_UNIX)
                self.s.connect(path)
                break
            except OSError:
                time.sleep(0.1)
        self.f = self.s.makefile("rw")
        self.f.readline()
        self.cmd("qmp_capabilities")

    def cmd(self, name, **args):
        self.f.write(json.dumps({"execute": name, "arguments": args}) + "\n")
        self.f.flush()
        while True:
            line = self.f.readline()
            if not line:
                return None
            msg = json.loads(line)
            if "return" in msg or "error" in msg:
                return msg

    def move(self, x, y):
        self.cmd("input-send-event", events=[
            {"type": "abs", "data": {"axis": "x", "value": int(x * 32767 / (W - 1))}},
            {"type": "abs", "data": {"axis": "y", "value": int(y * 32767 / (H - 1))}}])

    def button(self, down):
        self.cmd("input-send-event", events=[{"type": "btn", "data": {"down": down, "button": "left"}}])

    def key(self, qcode, shift=False):
        evs = []
        if shift:
            evs.append({"type": "key", "data": {"down": True, "key": {"type": "qcode", "data": "shift"}}})
        evs.append({"type": "key", "data": {"down": True, "key": {"type": "qcode", "data": qcode}}})
        self.cmd("input-send-event", events=evs)
        time.sleep(0.05)
        evs = [{"type": "key", "data": {"down": False, "key": {"type": "qcode", "data": qcode}}}]
        if shift:
            evs.append({"type": "key", "data": {"down": False, "key": {"type": "qcode", "data": "shift"}}})
        self.cmd("input-send-event", events=evs)
        time.sleep(0.05)


QCODES = {" ": "spc", "-": "minus", ".": "dot", ",": "comma", "/": "slash", "@": ("2", True), "!": ("1", True),
          ":": ("semicolon", True), ";": "semicolon", "=": "equal", "_": ("minus", True), "?": ("slash", True), "&": ("7", True)}


def main():
    args = sys.argv[1:]
    out = args.pop(0)
    stick = disk = vars_file = audio_wav = None
    debug = False
    while args and args[0].startswith("--"):
        opt = args.pop(0)
        if opt == "--stick":
            stick = args.pop(0)
        elif opt == "--disk":
            disk = args.pop(0)
        elif opt == "--vars":
            vars_file = args.pop(0)
        elif opt == "--debug":
            debug = True
        elif opt == "--audio-wav":
            audio_wav = args.pop(0)
    os.makedirs(out, exist_ok=True)
    if not vars_file:
        vars_file = os.path.join(out, "vars.fd")
    if not os.path.exists(vars_file):
        shutil.copy(os.environ.get("OVMF_VARS", "/usr/share/OVMF/OVMF_VARS_4M.fd"), vars_file)
    qmp_path = os.path.join(out, "qmp.sock")
    if os.path.exists(qmp_path):
        os.unlink(qmp_path)
    accel = "kvm" if os.path.exists("/dev/kvm") else "tcg"
    cmd = ["qemu-system-x86_64", "-machine", f"q35,accel={accel}", "-cpu", "max", "-m", "2G", "-smp", "2",
           "-drive", "if=pflash,format=raw,readonly=on,file=" + os.environ.get("OVMF_CODE", "/usr/share/OVMF/OVMF_CODE_4M.fd"),
           "-drive", f"if=pflash,format=raw,file={vars_file}",
           "-device", "virtio-vga", "-device", "qemu-xhci,id=xhci", "-device", "usb-tablet", "-device", "usb-kbd",
           "-display", "none", "-serial", "file:" + os.path.join(out, "serial.log"),
           "-qmp", f"unix:{qmp_path},server,nowait", "-no-reboot",
           "-nic", "user,model=virtio-net-pci"]
    if stick:
        cmd += ["-drive", f"if=none,id=stick,format=raw,file={stick},snapshot=on",
                "-device", "usb-storage,bus=xhci.0,drive=stick,removable=on,bootindex=1"]
    if disk:
        cmd += ["-drive", f"if=none,id=disk,format=raw,file={disk}", "-device", "virtio-blk-pci,drive=disk,bootindex=2"]
    if audio_wav:
        cmd += ["-audiodev", f"wav,id=snd,path={audio_wav}", "-device", "intel-hda", "-device", "hda-duplex,audiodev=snd"]
    if debug:
        cmd += ["-kernel", "build/kernel/arch/x86/boot/bzImage", "-append", "console=ttyS0 loglevel=7 zurbax.debug"]
    proc = subprocess.Popen(cmd)
    q = Qmp(qmp_path)
    try:
        for step in args:
            word, _, rest = step.partition(" ")
            if word == "wait":
                time.sleep(float(rest))
            elif word == "shot":
                ppm = os.path.join(out, rest + ".ppm")
                q.cmd("screendump", filename=ppm)
                time.sleep(0.5)
                subprocess.run(["convert", ppm, os.path.join(out, rest + ".png")], check=False)
                os.unlink(ppm)
                print("  shot", rest, flush=True)
            elif word == "click":
                x, y = map(float, rest.split())
                q.move(x, y)
                time.sleep(0.3)
                q.button(True)
                time.sleep(0.08)
                q.button(False)
                time.sleep(0.2)
            elif word == "type":
                for ch in rest:
                    code = QCODES.get(ch, ch.lower())
                    if isinstance(code, tuple):
                        q.key(code[0], code[1])
                    else:
                        q.key(code, ch.isupper())
            elif word == "key":
                q.key(rest)
            elif word == "waitoff":
                try:
                    proc.wait(float(rest))
                    print("  machine stopped", flush=True)
                except subprocess.TimeoutExpired:
                    print("  still running", flush=True)
    finally:
        if proc.poll() is None:
            q.cmd("quit")
            proc.wait(10)


if __name__ == "__main__":
    main()
