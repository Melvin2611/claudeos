#!/usr/bin/env python3
"""ClaudeOS QEMU test harness.

Boots the OS headless, drives it through QMP (keyboard, mouse, screenshots) and
checks the serial log.  Usage example:

  tools/qtest.py --args "kbd=us" wait:"desktop ready" shot:build/shot.png \
                 key:ctrl-alt-t sleep:1 type:"ls\\n" sleep:1 shot:build/term.png

Commands:
  wait:<regex>[@timeout]   wait until the serial log matches
  sleep:<seconds>
  shot:<file.png>          take a screenshot
  key:<combo>              press a key combo (qcodes joined by '-': ctrl-alt-t, ret, esc ...)
  type:<text>              type text (US layout; \\n = enter, \\t = tab)
  move:<x>,<y>             absolute mouse move (pixels)
  click:<x>,<y>[,btn]      move + click (btn: left/right/middle)
  dclick:<x>,<y>           double click
  drag:<x1>,<y1>,<x2>,<y2> drag with left button
  wheel:<n>                scroll wheel (+ down / - up)
  expect:<regex>           fail unless the serial log matches (no waiting)
  quit                     power off (hard)
"""
import argparse, json, os, re, shutil, socket, subprocess, sys, tempfile, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, "build")

US_MAP = {}
for c in "abcdefghijklmnopqrstuvwxyz":
    US_MAP[c] = [c]
    US_MAP[c.upper()] = ["shift", c]
for c in "0123456789":
    US_MAP[c] = [c]
for c, q in {" ": "spc", "\n": "ret", "\t": "tab", "-": "minus", "=": "equal", "[": "bracket_left",
             "]": "bracket_right", ";": "semicolon", "'": "apostrophe", "`": "grave_accent",
             "\\": "backslash", ",": "comma", ".": "dot", "/": "slash"}.items():
    US_MAP[c] = [q]
for c, q in {"!": "1", "@": "2", "#": "3", "$": "4", "%": "5", "^": "6", "&": "7", "*": "8", "(": "9",
             ")": "0", "_": "minus", "+": "equal", "{": "bracket_left", "}": "bracket_right",
             ":": "semicolon", '"': "apostrophe", "~": "grave_accent", "|": "backslash", "<": "comma",
             ">": "dot", "?": "slash"}.items():
    US_MAP[c] = ["shift", q]


class QMP:
    def __init__(self, path):
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        for _ in range(100):
            try:
                self.sock.connect(path)
                break
            except OSError:
                time.sleep(0.1)
        self.f = self.sock.makefile("rw")
        self.f.readline()
        self.cmd("qmp_capabilities")

    def cmd(self, name, **args):
        msg = {"execute": name}
        if args:
            msg["arguments"] = args
        self.f.write(json.dumps(msg) + "\n")
        self.f.flush()
        while True:
            line = self.f.readline()
            if not line:
                raise RuntimeError("QMP connection closed")
            r = json.loads(line)
            if "return" in r:
                return r["return"]
            if "error" in r:
                raise RuntimeError("QMP error: %s" % r["error"])


class Machine:
    def __init__(self, args="", mem="1G", uefi=False, kvm=True, disk=None, res=None, extra=None,
                 audio_wav=None, net=True, iso=None):
        self.tmp = tempfile.mkdtemp(prefix="qtest-")
        self.serial = os.path.join(self.tmp, "serial.log")
        self.qmp_path = os.path.join(self.tmp, "qmp.sock")
        iso = iso or self.make_iso(args, res)
        cmd = ["qemu-system-x86_64", "-machine", "pc", "-m", mem, "-vga", "std",
               "-display", "none", "-serial", "file:" + self.serial,
               "-qmp", "unix:%s,server,nowait" % self.qmp_path,
               "-cdrom", iso, "-boot", "d", "-rtc", "base=localtime", "-no-reboot"]
        if kvm and os.access("/dev/kvm", os.W_OK):
            cmd += ["-enable-kvm", "-cpu", "host"]
        if disk:
            cmd += ["-drive", "file=%s,format=raw,if=ide,index=0" % disk]
        if net:
            cmd += ["-nic", "user,model=e1000"]
        if audio_wav:
            cmd += ["-audiodev", "wav,id=snd0,path=" + audio_wav, "-device", "AC97,audiodev=snd0"]
        if uefi:
            vars_copy = os.path.join(self.tmp, "vars.fd")
            shutil.copy("/usr/share/edk2/x64/OVMF_VARS.4m.fd", vars_copy)
            cmd += ["-drive", "if=pflash,format=raw,readonly=on,file=/usr/share/edk2/x64/OVMF_CODE.4m.fd",
                    "-drive", "if=pflash,format=raw,file=" + vars_copy]
        if extra:
            cmd += extra
        self.proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        self.qmp = QMP(self.qmp_path)
        self.mx, self.my = 0, 0
        self.screen = None

    def make_iso(self, args, res):
        """Build a test ISO with a no-timeout grub.cfg carrying the given kernel args."""
        iso_dir = os.path.join(self.tmp, "iso")
        shutil.copytree(os.path.join(BUILD, "iso"), iso_dir)
        payload = "set gfxpayload=%s\n    " % res if res else ""
        with open(os.path.join(iso_dir, "boot/grub/grub.cfg"), "w") as f:
            f.write("set timeout=0\nset default=0\ninsmod all_video\n"
                    "menuentry test {\n    multiboot2 /boot/kernel.elf %s\n    %s"
                    "    module2 /boot/initrd.tar initrd\n    boot\n}\n" % (args, payload))
        iso = os.path.join(self.tmp, "test.iso")
        subprocess.run(["grub-mkrescue", "-o", iso, iso_dir], check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        subprocess.run([sys.executable, os.path.join(ROOT, "tools/fix-efi-nx.py"), iso], check=True,
                       stdout=subprocess.DEVNULL)
        return iso

    def log(self):
        try:
            with open(self.serial, "r", errors="replace") as f:
                return f.read()
        except FileNotFoundError:
            return ""

    def wait(self, pattern, timeout=30):
        end = time.time() + timeout
        rx = re.compile(pattern)
        while time.time() < end:
            if rx.search(self.log()):
                return True
            if self.proc.poll() is not None:
                break
            time.sleep(0.1)
        raise TimeoutError("pattern %r not found in serial log" % pattern)

    def shot(self, path):
        ppm = os.path.join(self.tmp, "shot.ppm")
        self.qmp.cmd("screendump", filename=ppm)
        for _ in range(50):
            if os.path.exists(ppm) and os.path.getsize(ppm) > 0:
                break
            time.sleep(0.05)
        time.sleep(0.1)
        from PIL import Image
        img = Image.open(ppm)
        self.screen = img.size
        os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
        img.save(path)
        os.remove(ppm)

    def key(self, combo, hold_ms=15):
        keys = [{"type": "qcode", "data": k} for k in combo.split("-")] if combo != "-" else \
               [{"type": "qcode", "data": "minus"}]
        self.qmp.cmd("send-key", keys=keys, **{"hold-time": hold_ms})
        time.sleep(0.03)

    def type(self, text):
        for ch in text:
            if ch not in US_MAP:
                continue
            self.key("-".join(US_MAP[ch]) if US_MAP[ch] != ["minus"] else "minus")

    def _screen_size(self):
        if not self.screen:
            self.shot(os.path.join(self.tmp, "size.png"))
        return self.screen

    def move(self, x, y):
        w, h = self._screen_size()
        ax = int(x * 0x7FFF / max(1, w - 1))
        ay = int(y * 0x7FFF / max(1, h - 1))
        self.qmp.cmd("input-send-event", events=[
            {"type": "abs", "data": {"axis": "x", "value": ax}},
            {"type": "abs", "data": {"axis": "y", "value": ay}}])
        self.mx, self.my = x, y
        time.sleep(0.05)

    def button(self, btn, down):
        self.qmp.cmd("input-send-event", events=[{"type": "btn", "data": {"down": down, "button": btn}}])
        time.sleep(0.05)

    def click(self, x, y, btn="left"):
        self.move(x, y)
        self.button(btn, True)
        self.button(btn, False)

    def dclick(self, x, y):
        self.click(x, y)
        self.click(x, y)

    def drag(self, x1, y1, x2, y2):
        self.move(x1, y1)
        self.button("left", True)
        steps = 10
        for i in range(1, steps + 1):
            self.move(x1 + (x2 - x1) * i // steps, y1 + (y2 - y1) * i // steps)
        self.button("left", False)

    def wheel(self, n):
        btn = "wheel-down" if n > 0 else "wheel-up"
        for _ in range(abs(n)):
            self.button(btn, True)
            self.button(btn, False)

    def quit(self):
        try:
            self.qmp.cmd("quit")
        except Exception:
            pass
        try:
            self.proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.proc.kill()

    def run_script(self, cmds):
        for c in cmds:
            op, _, arg = c.partition(":")
            if op == "wait":
                pat, _, t = arg.rpartition("@") if "@" in arg else (arg, "", "")
                self.wait(pat, float(t) if t else 30)
            elif op == "sleep":
                time.sleep(float(arg))
            elif op == "shot":
                self.shot(arg)
            elif op == "key":
                self.key(arg)
            elif op == "type":
                self.type(arg.encode().decode("unicode_escape"))
            elif op == "move":
                self.move(*map(int, arg.split(",")))
            elif op == "click":
                p = arg.split(",")
                self.click(int(p[0]), int(p[1]), p[2] if len(p) > 2 else "left")
            elif op == "dclick":
                self.dclick(*map(int, arg.split(",")))
            elif op == "drag":
                self.drag(*map(int, arg.split(",")))
            elif op == "wheel":
                self.wheel(int(arg))
            elif op == "expect":
                if not re.search(arg, self.log()):
                    raise AssertionError("expected %r in serial log" % arg)
            elif op == "quit":
                self.quit()
            else:
                raise ValueError("unknown command " + op)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--args", default="", help="kernel command line")
    ap.add_argument("--mem", default="1G")
    ap.add_argument("--uefi", action="store_true")
    ap.add_argument("--tcg", action="store_true")
    ap.add_argument("--disk")
    ap.add_argument("--res")
    ap.add_argument("--wav", help="capture audio to wav file")
    ap.add_argument("--nonet", action="store_true")
    ap.add_argument("--log", default=os.path.join(BUILD, "qtest-serial.log"))
    ap.add_argument("--iso")
    ap.add_argument("cmds", nargs="*")
    a = ap.parse_args()
    m = Machine(args=a.args, mem=a.mem, uefi=a.uefi, kvm=not a.tcg, disk=a.disk, res=a.res,
                audio_wav=a.wav, net=not a.nonet, iso=a.iso)
    rc = 0
    try:
        m.run_script(a.cmds)
    except Exception as e:
        print("FAILED:", e)
        rc = 1
    finally:
        m.quit()
        with open(a.log, "w") as f:
            f.write(m.log())
        err = m.proc.stderr.read().decode(errors="replace") if m.proc.stderr else ""
        if err.strip():
            print("qemu stderr:", err.strip()[:2000])
        shutil.rmtree(m.tmp, ignore_errors=True)
    print("---- serial log (tail) ----")
    print("\n".join(m_log_tail(a.log)))
    sys.exit(rc)


def m_log_tail(path, n=40):
    with open(path, errors="replace") as f:
        return f.read().splitlines()[-n:]


if __name__ == "__main__":
    main()
