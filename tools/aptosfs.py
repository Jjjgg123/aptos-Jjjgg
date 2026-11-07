#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
aptosfs.py — 在 Windows / Linux / macOS 上直接读写 aptos miniFS 镜像

支持：
  - 自动探测 minifs 分区起始 LBA
  - 列目录、读文件、导出文件到宿主、从宿主导入文件、建目录、删文件
  - 写操作完整维护位图与超级块 free_blocks（与内核 mfs_write 行为一致）
  - raw 镜像直接操作；VDI/VMDK 请先转换（VBoxManage convertfromraw --format RAW）

用法示例：
  python aptosfs.py -i G:\\aptos-os\\aptos-os406.img info
  python aptosfs.py -i os.img ls /
  python aptosfs.py -i os.img ls /DOCS
  python aptosfs.py -i os.img cat /DOCS/FIB.T
  python aptosfs.py -i os.img get /DOCS/FIB.T -o fib.t
  python aptosfs.py -i os.img put fib.t /DOCS/NEW.T
  python aptosfs.py -i os.img mkdir /DOCS/BIN
  python aptosfs.py -i os.img rm /DOCS/NEW.T
  python aptosfs.py -i os.img tree /DOCS

注意：写操作前请确保虚拟机已完全关闭，否则镜像被占用会失败。
"""

import argparse
import mmap
import os
import struct
import sys

SECTOR = 512
MAGIC = b"MINIFS02"
SB_FMT = "<8sIIIIIIIIII32s"          # 80 字节
ENT_FMT = "<40sIIIII4x"              # 64 字节（reserved 4 字节）
ENTRY_SZ = 64
PER_BLOCK = SECTOR // ENTRY_SZ        # 8
FLAG_DIR = 1
MAX_NAME = 39


class FsError(Exception):
    pass


class Entry(object):
    __slots__ = ("name", "size", "first", "flags", "mdate", "mtime")

    def __init__(self, name, size, first, flags, mdate, mtime):
        self.name = name
        self.size = size
        self.first = first
        self.flags = flags
        self.mdate = mdate
        self.mtime = mtime

    @property
    def is_dir(self):
        return bool(self.flags & FLAG_DIR)

    @property
    def blocks(self):
        if self.is_dir:
            return (self.size + PER_BLOCK - 1) // PER_BLOCK
        return (self.size + SECTOR - 1) // SECTOR


def _pack_entry(e):
    return struct.pack(ENT_FMT, e.name.encode("ascii", "replace")[:MAX_NAME].ljust(40, b"\x00")[:40],
                       e.size, e.first, e.flags, e.mdate, e.mtime)


def _unpack_entry(raw):
    (nm, size, first, flags, mdate, mtime) = struct.unpack(ENT_FMT, raw[:ENTRY_SZ])
    return Entry(nm.rstrip(b"\x00").decode("ascii", "replace"), size, first, flags, mdate, mtime)


def split_path(path):
    parts = [p for p in path.replace("\\", "/").split("/") if p and p != "."]
    return parts


class MiniFS(object):
    def __init__(self, path, fs_lba=None, writable=False):
        self.path = path
        self.f = open(path, "rb+" if writable else "rb")
        size = os.fstat(self.f.fileno()).st_size
        if size < 1024 * 1024:
            raise FsError("image too small: %d bytes" % size)
        self.mm = mmap.mmap(self.f.fileno(), 0,
                            access=mmap.ACCESS_WRITE if writable else mmap.ACCESS_READ)
        self.writable = writable
        self.fs_lba = fs_lba if fs_lba is not None else self._probe()
        self._load_super()

    # ---------- 基础块 IO ----------

    def _probe(self):
        total = len(self.mm) // SECTOR
        limit = min(total, 4096)
        for lba in range(limit):
            off = lba * SECTOR
            if self.mm[off:off + 8] == MAGIC:
                return lba
        raise FsError("MINIFS02 signature not found in first %d sectors; "
                      "pass --fs-lba explicitly" % limit)

    def _load_super(self):
        off = self.fs_lba * SECTOR
        (magic, bsz, total, bstart, bcnt, dstart, dcnt,
         sdata, sdatacnt, maxent, freecnt, label) = struct.unpack(SB_FMT, self.mm[off:off + 80])
        if magic != MAGIC:
            raise FsError("bad magic at LBA %d" % self.fs_lba)
        self.block_size = bsz
        self.total_blocks = total
        self.bitmap_start = bstart
        self.bitmap_blocks = bcnt
        self.dir_start = dstart
        self.dir_blocks = dcnt
        self.data_start = sdata
        self.data_blocks = sdatacnt
        self.max_entries = maxent
        self.free_blocks = freecnt
        self.label = label.rstrip(b"\x00").decode("ascii", "replace")
        if self.block_size != SECTOR:
            raise FsError("unsupported block size %d" % self.block_size)

    def _save_super(self):
        off = self.fs_lba * SECTOR
        raw = struct.pack(SB_FMT, MAGIC, self.block_size, self.total_blocks,
                          self.bitmap_start, self.bitmap_blocks, self.dir_start,
                          self.dir_blocks, self.data_start, self.data_blocks,
                          self.max_entries, self.free_blocks,
                          self.label.encode("ascii", "replace")[:32].ljust(32, b"\x00"))
        self.mm[off:off + 80] = raw

    def read_block(self, lba, count=1):
        off = lba * SECTOR
        return self.mm[off:off + count * SECTOR]

    def write_block(self, lba, data):
        if not self.writable:
            raise FsError("image opened read-only")
        off = lba * SECTOR
        self.mm[off:off + len(data)] = data

    # ---------- 位图 ----------

    def _bit_offset(self, rel):
        return self.bitmap_start * SECTOR + (rel >> 3)

    def bit_test(self, rel):
        return (self.mm[self._bit_offset(rel)] >> (rel & 7)) & 1

    def bit_set(self, rel, on=True):
        off = self._bit_offset(rel)
        b = self.mm[off]
        if on:
            b |= (1 << (rel & 7))
        else:
            b &= ~(1 << (rel & 7)) & 0xFF
        self.mm[off] = b

    def alloc_run(self, count):
        """first-fit 连续分配，返回绝对 LBA（与内核 alloc_blocks 一致）"""
        if not self.writable:
            raise FsError("image opened read-only")
        first_rel = self.data_start - self.fs_lba
        limit = self.total_blocks
        run = 0
        for i in range(first_rel, limit):
            if not self.bit_test(i):
                run += 1
                if run == count:
                    start = i + 1 - count
                    for j in range(count):
                        self.bit_set(start + j, True)
                    self.free_blocks -= count
                    return self.fs_lba + start
            else:
                run = 0
        raise FsError("not enough contiguous space for %d blocks" % count)

    def free_run(self, first_lba, count):
        if not first_lba or not count:
            return
        rel = first_lba - self.fs_lba
        for i in range(count):
            self.bit_set(rel + i, False)
        self.free_blocks += count

    # ---------- 目录 ----------

    def read_dir(self, start, entries):
        out = []
        for i in range(entries):
            off = start * SECTOR + i * ENTRY_SZ
            raw = self.mm[off:off + ENTRY_SZ]
            e = _unpack_entry(raw)
            if e.name and e.name != "":
                out.append((i, e))
        return out

    def write_entry(self, start, idx, e):
        off = start * SECTOR + idx * ENTRY_SZ
        self.mm[off:off + ENTRY_SZ] = _pack_entry(e)

    def clear_entry(self, start, idx):
        off = start * SECTOR + idx * ENTRY_SZ
        self.mm[off:off + ENTRY_SZ] = b"\x00" * ENTRY_SZ

    def _free_slot(self, start, entries):
        for i in range(entries):
            off = start * SECTOR + i * ENTRY_SZ
            if self.mm[off:off + 40].rstrip(b"\x00") == b"":
                return i
        return -1

    # ---------- 路径解析 ----------

    def resolve(self, path):
        """返回 (父目录 start, 父目录 entries, 父目录项下标(底线为 -1), Entry 或 None)"""
        parts = split_path(path)
        cur_start = self.dir_start
        cur_entries = self.max_entries
        parent = None
        parent_idx = -1
        for k, part in enumerate(parts):
            found = None
            found_idx = -1
            for idx, e in self.read_dir(cur_start, cur_entries):
                if e.name.upper() == part.upper():
                    found = e
                    found_idx = idx
                    break
            if found is None:
                return (cur_start, cur_entries, -1, None)
            parent = found
            parent_idx = found_idx
            if k == len(parts) - 1:
                return (cur_start, cur_entries, found_idx, found)
            if not found.is_dir:
                raise FsError("not a directory: %s" % part)
            cur_start = found.first
            cur_entries = found.size
        return (cur_start, cur_entries, -1, None)

    def _parent_of(self, path):
        parts = split_path(path)
        if not parts:
            raise FsError("empty path")
        if len(parts) == 1:
            return (self.dir_start, self.max_entries, None, parts[0])
        par = "/" + "/".join(parts[:-1])
        st, en, _, e = self.resolve(par)
        if e is None:
            raise FsError("parent directory not found: %s" % par)
        if not e.is_dir:
            raise FsError("parent is not a directory: %s" % par)
        return (e.first, e.size, e, parts[-1])

    # ---------- 对外操作 ----------

    def listdir(self, path):
        parts = split_path(path)
        if not parts:
            return self.read_dir(self.dir_start, self.max_entries)
        st, en, idx, e = self.resolve(path)
        if e is None:
            raise FsError("not found: %s" % path)
        if not e.is_dir:
            raise FsError("not a directory: %s" % path)
        return self.read_dir(e.first, e.size)

    def read_file(self, path):
        st, en, idx, e = self.resolve(path)
        if e is None:
            raise FsError("not found: %s" % path)
        if e.is_dir:
            raise FsError("is a directory: %s" % path)
        if e.size == 0:
            return b""
        nblocks = (e.size + SECTOR - 1) // SECTOR
        raw = self.mm[e.first * SECTOR:(e.first + nblocks) * SECTOR]
        return raw[:e.size]

    def write_file(self, path, data, mdate=20250101, mtime=1200):
        if not self.writable:
            raise FsError("image opened read-only")
        dstart, dentries, _, leaf = self._parent_of(path)
        exists = None
        exists_idx = -1
        for idx, e in self.read_dir(dstart, dentries):
            if e.name.upper() == leaf.upper():
                exists = e
                exists_idx = idx
                break

        if exists is not None and exists.is_dir:
            raise FsError("is a directory: %s" % path)

        nblocks = (len(data) + SECTOR - 1) // SECTOR
        new_first = 0
        if nblocks:
            new_first = self.alloc_run(nblocks)
            padded = data + b"\x00" * (nblocks * SECTOR - len(data))
            self.write_block(new_first, padded)

        if exists is not None:
            old_blocks = (exists.size + SECTOR - 1) // SECTOR
            if old_blocks:
                self.free_run(exists.first, old_blocks)
            ne = Entry(leaf.upper(), len(data), new_first, 0, mdate, mtime)
            self.write_entry(dstart, exists_idx, ne)
        else:
            slot = self._free_slot(dstart, dentries)
            if slot < 0:
                slot = self._grow_dir(dstart, dentries)
            ne = Entry(leaf.upper(), len(data), new_first, 0, mdate, mtime)
            self.write_entry(dstart, slot, ne)
        self._save_super()

    def mkdir(self, path, mdate=20250101, mtime=1200):
        if not self.writable:
            raise FsError("image opened read-only")
        dstart, dentries, _, leaf = self._parent_of(path)
        for idx, e in self.read_dir(dstart, dentries):
            if e.name.upper() == leaf.upper():
                raise FsError("already exists: %s" % path)
        first = self.alloc_run(1)
        self.write_block(first, b"\x00" * SECTOR)
        slot = self._free_slot(dstart, dentries)
        if slot < 0:
            slot = self._grow_dir(dstart, dentries)
        ne = Entry(leaf.upper(), PER_BLOCK, first, FLAG_DIR, mdate, mtime)
        self.write_entry(dstart, slot, ne)
        self._save_super()

    def delete(self, path):
        if not self.writable:
            raise FsError("image opened read-only")
        parts = split_path(path)
        if not parts:
            raise FsError("cannot delete root")
        st, en, idx, e = self.resolve(path)
        if e is None:
            raise FsError("not found: %s" % path)
        if e.is_dir:
            if self.read_dir(e.first, e.size):
                raise FsError("directory not empty: %s" % path)
        self.free_run(e.first, e.blocks)
        self.clear_entry(st, idx)
        self._save_super()

    def _grow_dir(self, start, entries):
        """目录满时扩容：新分配 2x 块，搬过去，更新父目录项"""
        old_blocks = (entries + PER_BLOCK - 1) // PER_BLOCK
        new_entries = entries * 2
        new_blocks = (new_entries + PER_BLOCK - 1) // PER_BLOCK
        if new_entries > 4096:
            raise FsError("directory entry limit reached")
        new_first = self.alloc_run(new_blocks)
        old_raw = self.read_block(start, old_blocks)
        self.write_block(new_first, old_raw + b"\x00" * (new_blocks - old_blocks) * SECTOR)
        self.free_run(start, old_blocks)
        # 找到指回这个目录的父项并更新
        self._retarget(start, new_first, new_entries)
        return new_entries

    def _retarget(self, old_first, new_first, new_entries):
        """在根和所有子目录里查找指向 old_first 的项并改写"""
        def walk(start, entries, depth):
            if depth > 8:
                return False
            for idx, e in self.read_dir(start, entries):
                if e.is_dir:
                    if e.first == old_first:
                        e.first = new_first
                        e.size = new_entries
                        self.write_entry(start, idx, e)
                        return True
                    if walk(e.first, e.size, depth + 1):
                        return True
            return False
        return walk(self.dir_start, self.max_entries, 0)

    def info(self):
        def pct(a, b):
            return 100.0 * a / b if b else 0.0
        return {
            "image": self.path,
            "fs_lba": self.fs_lba,
            "label": self.label,
            "block_size": self.block_size,
            "total_blocks": self.total_blocks,
            "bitmap_start": self.bitmap_start,
            "bitmap_blocks": self.bitmap_blocks,
            "dir_start": self.dir_start,
            "dir_blocks": self.dir_blocks,
            "max_entries": self.max_entries,
            "data_start": self.data_start,
            "data_blocks": self.data_blocks,
            "free_blocks": self.free_blocks,
            "used_blocks": self.total_blocks - self.free_blocks,
            "usage_pct": pct(self.total_blocks - self.free_blocks, self.total_blocks),
        }

    def close(self):
        try:
            if self.writable:
                self.mm.flush()
            self.mm.close()
        finally:
            self.f.close()


# ---------------- CLI ----------------

def fmt_size(n):
    if n < 1024:
        return "%d B" % n
    if n < 1024 * 1024:
        return "%.1f KB" % (n / 1024.0)
    return "%.1f MB" % (n / 1048576.0)


def cmd_info(fs, args):
    d = fs.info()
    print("image          : %s" % d["image"])
    print("fs partition   : LBA %d (offset %d)" % (d["fs_lba"], d["fs_lba"] * SECTOR))
    print("volume label   : %s" % d["label"])
    print("block size     : %d" % d["block_size"])
    print("total blocks   : %d (%s)" % (d["total_blocks"], fmt_size(d["total_blocks"] * SECTOR)))
    print("bitmap         : %d blocks @ LBA %d" % (d["bitmap_blocks"], d["bitmap_start"]))
    print("root dir       : %d blocks @ LBA %d, %d entries"
          % (d["dir_blocks"], d["dir_start"], d["max_entries"]))
    print("data area      : %d blocks @ LBA %d (%s)"
          % (d["data_blocks"], d["data_start"], fmt_size(d["data_blocks"] * SECTOR)))
    print("free blocks    : %d of %d  (%.1f%% used)"
          % (d["free_blocks"], d["total_blocks"], d["usage_pct"]))
    # 目录树规模
    files = dirs = 0
    stack = [(fs.dir_start, fs.max_entries)]
    seen = 0
    while stack and seen < 4096:
        st, en = stack.pop()
        for _, e in fs.read_dir(st, en):
            seen += 1
            if e.is_dir:
                dirs += 1
                stack.append((e.first, e.size))
            else:
                files += 1
    print("contents       : %d files, %d dirs" % (files, dirs))


def cmd_ls(fs, args):
    path = args.path or "/"
    items = fs.listdir(path)
    if args.long:
        print("%-10s %-10s %5s  %s" % ("SIZE", "TYPE", "BLOCK", "NAME"))
    for _, e in items:
        if args.long:
            print("%-10s %-10s %5d  %s" % (fmt_size(e.size) if not e.is_dir else "<DIR>",
                                           "dir" if e.is_dir else "file",
                                           e.first, e.name))
        else:
            print(("%-24s" % e.name) + ("  <DIR>" if e.is_dir else ""))
    if not items:
        print("(empty)")


def cmd_cat(fs, args):
    data = fs.read_file(args.path)
    sys.stdout.write(data.decode("utf-8", "replace"))
    if not data.endswith(b"\n"):
        sys.stdout.write("\n")


def cmd_get(fs, args):
    data = fs.read_file(args.path)
    out = args.out or os.path.basename(args.path.replace("\\", "/"))
    with open(out, "wb") as f:
        f.write(data)
    print("-> %s (%s)" % (out, fmt_size(len(data))))


def cmd_put(fs, args):
    with open(args.src, "rb") as f:
        data = f.read()
    fs.write_file(args.dest, data)
    print("-> %s (%s) written to %s" % (args.src, fmt_size(len(data)), args.dest))


def cmd_mkdir(fs, args):
    fs.mkdir(args.path)
    print("created %s" % args.path)


def cmd_rm(fs, args):
    fs.delete(args.path)
    print("removed %s" % args.path)


def cmd_tree(fs, args):
    root = args.path or "/"

    def walk(path, prefix):
        try:
            items = fs.listdir(path)
        except FsError as ex:
            print(prefix + "[%s]" % ex)
            return
        for _, e in items:
            p = path.rstrip("/") + "/" + e.name
            if e.is_dir:
                print(prefix + e.name + "/")
                walk(p, prefix + "  ")
            else:
                print(prefix + "%-30s %s" % (e.name, fmt_size(e.size)))

    print(root)
    walk(root, "  ")


def main():
    ap = argparse.ArgumentParser(description="aptos miniFS image tool")
    ap.add_argument("-i", "--image", required=True, help="镜像路径 (.img)")
    ap.add_argument("--fs-lba", type=int, default=None, help="手工指定 FS 起始 LBA")
    ap.add_argument("-w", "--write", action="store_true", help="以读写方式打开（需要）")

    sub = ap.add_subparsers(dest="cmd")

    p = sub.add_parser("info", help="显示超级块与容量")
    p.set_defaults(func=cmd_info)

    p = sub.add_parser("ls", help="列目录")
    p.add_argument("path", nargs="?")
    p.add_argument("-l", "--long", action="store_true")
    p.set_defaults(func=cmd_ls)

    p = sub.add_parser("cat", help="输出文件内容到标准输出")
    p.add_argument("path")
    p.set_defaults(func=cmd_cat)

    p = sub.add_parser("get", help="从镜像导出文件")
    p.add_argument("path")
    p.add_argument("-o", "--out")
    p.set_defaults(func=cmd_get)

    p = sub.add_parser("put", help="把宿主文件写入镜像")
    p.add_argument("src")
    p.add_argument("dest")
    p.set_defaults(func=cmd_put)

    p = sub.add_parser("mkdir", help="创建目录")
    p.add_argument("path")
    p.set_defaults(func=cmd_mkdir)

    p = sub.add_parser("rm", help="删除文件或空目录")
    p.add_argument("path")
    p.set_defaults(func=cmd_rm)

    p = sub.add_parser("tree", help="递归列出")
    p.add_argument("path", nargs="?")
    p.set_defaults(func=cmd_tree)

    args = ap.parse_args()
    if not args.cmd:
        args.cmd = "info"
        args.func = cmd_info

    write_cmds = {"put", "mkdir", "rm"}
    need_write = args.write or args.cmd in write_cmds

    if need_write and not os.access(args.image, os.W_OK):
        print("error: image is not writable", file=sys.stderr)
        return 2

    try:
        fs = MiniFS(args.image, fs_lba=args.fs_lba, writable=need_write)
    except FsError as ex:
        print("error: %s" % ex, file=sys.stderr)
        return 2
    except PermissionError:
        print("error: cannot open %s (在虚拟机里被占用？先完全关闭虚拟机)" % args.image,
              file=sys.stderr)
        return 2

    try:
        rc = args.func(fs, args) or 0
    finally:
        fs.close()
    return rc


if __name__ == "__main__":
    sys.exit(main())
