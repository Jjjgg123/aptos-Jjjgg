import argparse
import math
import os
import struct
import sys

SECTOR = 512
STAGE2_LBA = 1
STAGE2_SECTORS = 8
KERNEL_LBA = STAGE2_LBA + STAGE2_SECTORS

MINIFS_MAGIC = b"MINIFS02"
DIR_BLOCKS = 32
ENTRY_SIZE = 64
ENTRY_NAME = 40
MAX_NAME = ENTRY_NAME - 1
PER_BLOCK = SECTOR // ENTRY_SIZE


def align_up(value, align):
    return (value + align - 1) // align * align


def patch(blob, marker, values):
    off = blob.find(marker)
    if off < 0:
        raise SystemExit("mkimg: marker %r not found" % marker)
    out = bytearray(blob)
    pos = off + len(marker)
    for fmt, val in values:
        size = struct.calcsize(fmt)
        out[pos:pos + size] = struct.pack(fmt, val)
        pos += size
    return bytes(out)


def dos_name(name):
    name = name.strip().upper().replace("/", "_").replace("\\", "_")
    return name[:MAX_NAME]


def norm(path):
    parts = []
    for part in path.split("/"):
        if part in ("", "."):
            continue
        if part == "..":
            if parts:
                parts.pop()
            continue
        parts.append(part)
    return "/" + "/".join(parts)


def parent_of(path):
    path = norm(path)
    if path == "/":
        return "/"
    return norm(path.rsplit("/", 1)[0] or "/")


def base_of(path):
    path = norm(path)
    if path == "/":
        return "/"
    return path.rsplit("/", 1)[1]


class MiniFS:
    def __init__(self, image, fs_start):
        self.image = image
        self.fs_start = fs_start
        total_blocks = (len(image) // SECTOR) - fs_start
        self.total_blocks = total_blocks
        self.bitmap_blocks = align_up(align_up(total_blocks, 8) // 8, SECTOR) // SECTOR
        self.bitmap_start = fs_start + 1
        self.dir_start = self.bitmap_start + self.bitmap_blocks
        self.data_start = self.dir_start + DIR_BLOCKS
        self.data_blocks = total_blocks - 1 - self.bitmap_blocks - DIR_BLOCKS
        self.max_entries = DIR_BLOCKS * PER_BLOCK
        self.bitmap = bytearray(self.bitmap_blocks * SECTOR)
        self.dirs = {"/": []}
        self.subdir_entry = {}
        self.next_free = self.data_start
        for blk in range(self.data_start - self.fs_start):
            self.set_used(blk)

    def set_used(self, index):
        self.bitmap[index // 8] |= 1 << (index % 8)

    def write_block(self, block, data):
        off = block * SECTOR
        self.image[off:off + len(data)] = data

    def alloc(self, count):
        start = self.next_free
        if start + count > self.fs_start + self.total_blocks:
            raise SystemExit("mkimg: filesystem is full")
        for i in range(count):
            self.set_used(start - self.fs_start + i)
        self.next_free = start + count
        return start

    def add_dir(self, path):
        path = norm(path)
        parent = parent_of(path)
        if path in self.subdir_entry:
            return self.subdir_entry[path]
        self.dirs.setdefault(parent, [])
        entry = {"name": dos_name(base_of(path)), "dir": True, "size": 0, "first": 0}
        self.dirs[parent].append(entry)
        self.dirs.setdefault(path, [])
        self.subdir_entry[path] = entry
        return entry

    def add_file(self, path, content, date=20250101, time=1200):
        path = norm(path)
        parent = parent_of(path)
        self.dirs.setdefault(parent, [])
        entry = {"name": dos_name(base_of(path)), "dir": False, "size": len(content),
                 "first": 0, "content": content, "date": date, "time": time}
        self.dirs[parent].append(entry)
        return entry

    def finish(self, label="aptos"):
        for path, entries in self.dirs.items():
            for e in entries:
                if e["dir"]:
                    continue
                blocks = max(1, math.ceil(e["size"] / SECTOR))
                first = self.alloc(blocks)
                e["first"] = first
                self.write_block(first, e["content"] + b"\x00" * (blocks * SECTOR - e["size"]))

        for path in sorted(self.dirs):
            if path == "/":
                continue
            entries = self.dirs[path]
            blocks = max(1, math.ceil(len(entries) / PER_BLOCK))
            first = self.alloc(blocks)
            entry = self.subdir_entry[path]
            entry["first"] = first
            entry["size"] = blocks * PER_BLOCK
            self.write_block(first, b"\x00" * (blocks * SECTOR))

        for path, entries in self.dirs.items():
            blob = bytearray()
            for e in entries:
                if e["dir"]:
                    blob += struct.pack("<40sIIIII", e["name"].encode(), e["size"],
                                        e["first"], 1, 20250101, 1200)
                else:
                    blob += struct.pack("<40sIIIII", e["name"].encode(), e["size"],
                                        e["first"], 0, e.get("date", 20250101),
                                        e.get("time", 1200))
                blob += b"\x00" * (ENTRY_SIZE - 60)
            if path == "/":
                if len(entries) > self.max_entries:
                    raise SystemExit("mkimg: root directory is full")
                blob += b"\x00" * (DIR_BLOCKS * SECTOR - len(blob))
                for i in range(DIR_BLOCKS):
                    self.write_block(self.dir_start + i,
                                     bytes(blob[i * SECTOR:(i + 1) * SECTOR]))
            else:
                first = self.subdir_entry[path]["first"]
                blocks = self.subdir_entry[path]["size"] // PER_BLOCK
                blob += b"\x00" * (blocks * SECTOR - len(blob))
                for i in range(blocks):
                    self.write_block(first + i, bytes(blob[i * SECTOR:(i + 1) * SECTOR]))

        sb = struct.pack(
            "<8sIIIIIIIIII32s",
            MINIFS_MAGIC,
            SECTOR,
            self.total_blocks,
            self.bitmap_start,
            self.bitmap_blocks,
            self.dir_start,
            DIR_BLOCKS,
            self.data_start,
            self.data_blocks,
            self.max_entries,
            self.data_blocks - (self.next_free - self.data_start),
            label.encode()[:32].ljust(32, b"\x00"),
        )
        self.write_block(self.fs_start, sb.ljust(SECTOR, b"\x00"))
        for i in range(self.bitmap_blocks):
            self.write_block(self.bitmap_start + i,
                             bytes(self.bitmap[i * SECTOR:(i + 1) * SECTOR]))


SAMPLE_TREE = [
    ("dir", "/DOCS", None),
    ("dir", "/DOCS/GUIDE", None),
    ("dir", "/PICS", None),
    ("dir", "/MNT", None),
    ("file", "/README.TXT", """aptos - a tiny 32-bit operating system
=======================================

You are looking at a real (if small) operating system:

  * custom two-stage bootloader, no GRUB
  * 32-bit protected mode kernel written in C
  * VBE linear framebuffer graphics + window manager
  * PS/2 keyboard and mouse drivers
  * ATA PIO disk driver
  * MINIFS02, a hierarchical filesystem

This disk image now has real directories:

  /README.TXT          this file
  /HELLO.TXT           a greeting
  /DOCS/               nested files
  /DOCS/GUIDE/HOWTO.TXT
  /PICS/               empty, make it yours
  /MNT/                mount point for other disks

Attach a second disk in QEMU and try:

  dev                  list the block devices
  mkfs hd1             format it
  mount hd1 /MNT/D2    mount it
  ls /MNT/D2           look inside
  umount /MNT/D2

Try the Terminal app:

  help                 list every command
  ls /DOCS             list a directory
  cd /DOCS/GUIDE       walk around
  pwd
  cat HOWTO.TXT        read a file
  mkdir /PICS/LOGO     create a directory
  cp HOWTO.TXT COPY.TXT
  mv COPY.TXT RENAMED.TXT
  rm RENAMED.TXT

Everything you write lands on this very disk image.
"""),
    ("file", "/HELLO.TXT", "Hello from the aptos filesystem!\nWritten into os.img by tools/mkimg.py.\n"),
    ("file", "/DOCS/NOTES.TXT", """Todo list for the next version
-----------------------------
  [x] bootloader + protected mode
  [x] framebuffer graphics and window manager
  [x] keyboard, mouse, timer interrupts
  [x] ATA disk driver
  [x] hierarchical filesystem with directories
  [ ] paging and a real user mode
  [ ] networking
  [ ] preemptive multitasking
"""),
    ("file", "/DOCS/TODO.TXT", "1. write a scheduler\n2. add paging\n3. port DOOM\n"),
    ("file", "/DOCS/GUIDE/HOWTO.TXT", """How to drive this filesystem
============================

Paths are absolute or relative to the shell's working directory.

  cd /DOCS          enter a directory
  cd ..             go back up
  cd /              jump to the root
  pwd               show where you are
  ls                list the current directory
  ls /DOCS/GUIDE    list any other directory
  mkdir NAME        create a directory
  write NAME text   create or overwrite a file
  cat NAME          print a file
  cp A B            copy
  mv A B            rename or move
  rm NAME           delete a file, or an empty directory

The Files app in the GUI can walk the same tree with the mouse.
"""),
]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True)
    ap.add_argument("--stage1")
    ap.add_argument("--stage2")
    ap.add_argument("--kernel")
    ap.add_argument("--size-mb", type=int, default=64)
    ap.add_argument("--size-kb", type=int, default=0)
    ap.add_argument("--label", default="aptos")
    ap.add_argument("--fs-only", action="store_true")
    ap.add_argument("--boot-only", action="store_true")
    ap.add_argument("--apps", default="")
    ap.add_argument("--install", action="append", default=[])
    ap.add_argument("--add", action="append", default=[])
    args = ap.parse_args()

    total = args.size_kb * 1024 if args.size_kb else args.size_mb * 1024 * 1024
    image = bytearray(total)

    if args.fs_only:
        fs = MiniFS(image, 0)
        tree = list(SAMPLE_TREE)
        mkdirs = set()
        for spec in args.add:
            path, _, src = spec.partition("=")
            parts = path.strip("/").split("/")
            if len(parts) > 1:
                cur = ""
                for part in parts[:-1]:
                    cur += "/" + part
                    if cur not in mkdirs:
                        tree.append(("dir", cur, None))
                        mkdirs.add(cur)
            tree.append(("file", path, open(src, "rb").read()))
        if args.install:
            tree.append(("dir", "/APPS", None))
        for spec in args.install:
            name, _, path = spec.partition("=")
            tree.append(("file", "/APPS/%s.APP" % name.upper(), open(path, "rb").read()))
        if args.apps:
            tree.append(("file", "/APPS.LST", open(args.apps, "rb").read()))
        for kind, path, content in tree:
            if kind == "dir":
                fs.add_dir(path)
            else:
                fs.add_file(path, content.encode() if isinstance(content, str) else content)
        fs.finish(args.label)
        open(args.out, "wb").write(image)
        print("mkimg: %s (data-only %d KiB, label %s)" % (args.out, total // 1024, args.label))
        for kind, path, _ in SAMPLE_TREE:
            print("    %s %s" % ("d" if kind == "dir" else "-", path))
        return

    stage1 = open(args.stage1, "rb").read()
    stage2 = open(args.stage2, "rb").read()
    kernel = open(args.kernel, "rb").read()

    if len(stage1) != SECTOR:
        raise SystemExit("mkimg: stage1 must be exactly 512 bytes")
    if len(stage2) > STAGE2_SECTORS * SECTOR:
        raise SystemExit("mkimg: stage2 does not fit in %d sectors" % STAGE2_SECTORS)
    if 0x10000 + len(kernel) > 0x9FC00:
        raise SystemExit("mkimg: kernel too large (%d bytes, max %d)"
                         % (len(kernel), 0x9FC00 - 0x10000))

    kernel_sectors = align_up(len(kernel), SECTOR) // SECTOR
    fs_lba = KERNEL_LBA + kernel_sectors

    if args.boot_only:
        stage1 = patch(stage1, b"MYO1", [("<I", STAGE2_LBA), ("<H", STAGE2_SECTORS)])
        stage2 = patch(stage2, b"MYO2", [("<I", KERNEL_LBA), ("<H", kernel_sectors),
                                         ("<I", fs_lba)])
        image[0:SECTOR] = stage1
        off = STAGE2_LBA * SECTOR
        image[off:off + len(stage2)] = stage2
        koff = KERNEL_LBA * SECTOR
        image[koff:koff + len(kernel)] = kernel
        open(args.out, "wb").write(image)
        print("mkimg: %s (boot-only %d KiB; stage2 LBA %d x%d, kernel LBA %d x%d)"
              % (args.out, total // 1024, STAGE2_LBA, STAGE2_SECTORS, KERNEL_LBA,
                 kernel_sectors))
        return

    stage1 = patch(stage1, b"MYO1", [("<I", STAGE2_LBA), ("<H", STAGE2_SECTORS)])
    stage2 = patch(stage2, b"MYO2", [("<I", KERNEL_LBA), ("<H", kernel_sectors), ("<I", fs_lba)])

    image[0:SECTOR] = stage1
    off = STAGE2_LBA * SECTOR
    image[off:off + len(stage2)] = stage2
    off = KERNEL_LBA * SECTOR
    image[off:off + len(kernel)] = kernel

    fs = MiniFS(image, fs_lba)
    tree = list(SAMPLE_TREE)
    mkdirs = set()
    for spec in args.add:
        path, _, src = spec.partition("=")
        parts = path.strip("/").split("/")
        if len(parts) > 1:
            cur = ""
            for part in parts[:-1]:
                cur += "/" + part
                if cur not in mkdirs:
                    tree.append(("dir", cur, None))
                    mkdirs.add(cur)
        tree.append(("file", path, open(src, "rb").read()))
    if args.install:
        tree.append(("dir", "/APPS", None))
    for spec in args.install:
        name, _, path = spec.partition("=")
        tree.append(("file", "/APPS/%s.APP" % name.upper(), open(path, "rb").read()))
    if args.apps:
        tree.append(("file", "/APPS.LST", open(args.apps, "rb").read()))
    for kind, path, content in tree:
        if kind == "dir":
            fs.add_dir(path)
        else:
            fs.add_file(path, content.encode() if isinstance(content, str) else content)
    fs.finish(args.label)

    with open(args.out, "wb") as f:
        f.write(image)

    print("mkimg: %s  (%d MiB)" % (args.out, args.size_mb))
    print("  stage1      sector 0")
    print("  stage2      sector %d..%d" % (STAGE2_LBA, STAGE2_LBA + STAGE2_SECTORS - 1))
    print("  kernel      sector %d..%d  (%d bytes)" % (KERNEL_LBA, fs_lba - 1, len(kernel)))
    print("  %s    sector %d  (%d blocks, %d data blocks, root holds %d)"
          % (MINIFS_MAGIC.decode(), fs_lba, fs.total_blocks, fs.data_blocks, fs.max_entries))
    for kind, path, _ in SAMPLE_TREE:
        print("    %s %s" % ("d" if kind == "dir" else "-", path))


if __name__ == "__main__":
    main()
