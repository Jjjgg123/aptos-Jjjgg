import argparse
import os
import struct
import subprocess
import sys
import threading
import time
import zlib

KEYMAP = {
    " ": "spc",
    "\n": "ret",
    "\t": "tab",
    ".": "dot",
    ",": "comma",
    "/": "slash",
    "-": "minus",
    "=": "equal",
    ";": "semicolon",
    "'": "apostrophe",
    "[": "bracket_left",
    "]": "bracket_right",
    "\\": "backslash",
    "`": "grave_accent",
    "*": "shift-8",
    "+": "shift-equal",
    "_": "shift-minus",
    ":": "shift-semicolon",
    "!": "shift-1",
    "?": "shift-slash",
    ">": "shift-dot",
    "<": "shift-comma",
    '"': "shift-apostrophe",
    "(": "shift-9",
    ")": "shift-0",
}

def key_for(ch):
    if ch in KEYMAP:
        return KEYMAP[ch]
    if "a" <= ch <= "z" or "0" <= ch <= "9":
        return ch
    if "A" <= ch <= "Z":
        return "shift-" + ch.lower()
    raise SystemExit("qemu_test: no key mapping for %r" % ch)

def ppm_to_png(ppm_path, png_path):
    data = open(ppm_path, "rb").read()
    if not data.startswith(b"P6"):
        raise SystemExit("qemu_test: %s is not a P6 PPM" % ppm_path)
    fields = []
    i = 2
    while len(fields) < 3:
        while i < len(data) and data[i:i + 1].isspace():
            i += 1
        if data[i:i + 1] == b"#":
            while data[i:i + 1] not in (b"\n", b""):
                i += 1
            continue
        start = i
        while i < len(data) and not data[i:i + 1].isspace():
            i += 1
        fields.append(int(data[start:i]))
    i += 1
    width, height, maxval = fields
    px = data[i:i + width * height * 3]

    raw = bytearray()
    stride = width * 3
    for y in range(height):
        raw.append(0)
        raw += px[y * stride:(y + 1) * stride]

    def chunk(tag, payload):
        return (struct.pack(">I", len(payload)) + tag + payload +
                struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF))

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(raw), 6))
    png += chunk(b"IEND", b"")
    open(png_path, "wb").write(png)

class Qemu:
    def __init__(self, image, memory, serial, extra, cdrom=False, disk=""):
        cmd = [
            "qemu-system-i386", "-m", str(memory), "-vga", "std",
        ]
        if cdrom:
            cmd += ["-cdrom", image, "-boot", "d"]
            if disk:
                cmd += ["-drive", "file=%s,format=raw,index=0,media=disk" % disk]
            else:
                cmd += ["-drive", "file=/tmp/aptos-blank.img,format=raw,index=0,media=disk"]
        else:
            cmd += ["-drive", "file=%s,format=raw,index=0,media=disk" % image]
            if disk:
                cmd += ["-drive", "file=%s,format=raw,index=1,media=disk" % disk]
        cmd += [
            "-display", "none", "-monitor", "stdio",
            "-serial", "file:%s" % serial,
            "-rtc", "base=localtime", "-no-reboot",
        ] + extra
        self.log = open(serial.replace(".log", ".qemu.log"), "wb")
        self.proc = subprocess.Popen(
            cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT)
        self.reader = threading.Thread(target=self._drain, daemon=True)
        self.reader.start()

    def _drain(self):
        try:
            for line in self.proc.stdout:
                self.log.write(line)
                self.log.flush()
        except Exception:
            pass

    def cmd(self, text, wait=0.25):
        self.proc.stdin.write((text + "\n").encode())
        self.proc.stdin.flush()
        time.sleep(wait)

    def stop(self):
        try:
            self.cmd("quit", 0.1)
            self.proc.wait(timeout=3)
        except Exception:
            self.proc.kill()

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("image")
    ap.add_argument("--memory", type=int, default=128)
    ap.add_argument("--serial", default="build/serial.log")
    ap.add_argument("--step", action="append", default=[])
    ap.add_argument("--qemu-arg", action="append", default=[])
    ap.add_argument("--cdrom", action="store_true",
                    help="attach the image as a CD-ROM and boot from it")
    ap.add_argument("--disk", default="", help="extra hard disk image")
    args = ap.parse_args()

    q = Qemu(args.image, args.memory, args.serial, args.qemu_arg,
             cdrom=args.cdrom, disk=args.disk)
    t_start = time.time()
    try:
        time.sleep(1.0)
        for step in args.step:
            kind, _, value = step.partition(":")
            if kind == "delay":
                time.sleep(float(value))
            elif kind == "shot":
                ppm = value[:-4] + ".ppm" if value.endswith(".png") else value
                q.cmd("screendump %s" % ppm, 0.7)
                if value.endswith(".png"):
                    ppm_to_png(ppm, value)
                    os.unlink(ppm)
                print("[shot] %s  t=%.1fs" % (value, time.time() - t_start))
            elif kind == "type":
                for ch in value:
                    q.cmd("sendkey %s" % key_for(ch), 0.06)
                time.sleep(0.3)
            elif kind == "key":
                q.cmd("sendkey %s" % value, 0.15)
            elif kind == "move":
                dx, dy = value.split(",")
                q.cmd("mouse_move %s %s" % (dx, dy), 0.15)
            elif kind == "click":
                q.cmd("mouse_button %s" % value, 0.15)
            elif kind == "mark":
                print("[mark] %s" % value)
            elif kind == "mon":
                q.cmd(value, 0.6)
            else:
                raise SystemExit("qemu_test: unknown step %r" % step)
    finally:
        q.stop()
        time.sleep(0.3)

    if os.path.exists(args.serial):
        tail = open(args.serial, "rb").read().decode("utf-8", "replace")
        print("--- serial (%d bytes) ---" % len(tail))
        print(tail)

if __name__ == "__main__":
    main()
