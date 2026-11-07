import math
import struct
import sys

SECTOR = 512
ENTRY = 64


def find_fs(d):
    off = d.find(b"MYO2")
    if off < 0:
        raise SystemExit("lsimg: no MYO2 patch block, not an aptos image")
    kernel_lba, kernel_sec, fs_lba = struct.unpack_from("<IHI", d, off + 4)
    magic = d[fs_lba * SECTOR:fs_lba * SECTOR + 8]
    if magic != b"MINIFS02":
        raise SystemExit("lsimg: sector %d holds %r, not MINIFS02" % (fs_lba, magic))
    return fs_lba


def read_dir(d, start, entries):
    out = []
    for i in range(entries):
        off = start * SECTOR + i * ENTRY
        name, size, first, flags, mdate, mtime, _ = struct.unpack_from("<40sIIIIII", d, off)
        if not name[0]:
            continue
        out.append({
            "name": name.split(b"\0")[0].decode("ascii", "replace"),
            "size": size,
            "first": first,
            "dir": bool(flags & 1),
            "mdate": mdate,
            "mtime": mtime,
        })
    return out


def walk(d, start, entries, prefix, stats):
    for e in read_dir(d, start, entries):
        if e["dir"]:
            stats["dirs"] += 1
            print("%s%-16s <DIR>" % (prefix, e["name"]))
            walk(d, e["first"], e["size"], prefix + "  ", stats)
        else:
            stats["files"] += 1
            stats["bytes"] += e["size"]
            print("%s%-16s %7d bytes   %d/%02d" % (prefix, e["name"], e["size"],
                                                   e["mdate"], e["mtime"]))


def main():
    if len(sys.argv) < 2:
        raise SystemExit("usage: lsimg.py <image>")
    d = open(sys.argv[1], "rb").read()
    fs_lba = find_fs(d)
    sb = struct.unpack_from("<8sIIIIIIIIII32s", d, fs_lba * SECTOR)
    magic, block, total, bmp_start, bmp_blocks, dir_start, dir_blocks, \
        data_start, data_blocks, max_entries, free_blocks, label = sb

    print("image      %s" % sys.argv[1])
    print("filesystem %s  label '%s'  at sector %d"
          % (magic.decode(), label.split(b"\0")[0].decode(), fs_lba))
    print("total      %d KiB   free %d KiB   root holds %d entries"
          % (total // 2, free_blocks // 2, max_entries))
    print("")

    stats = {"files": 0, "dirs": 0, "bytes": 0}
    walk(d, dir_start, max_entries, "  ", stats)
    print("")
    print("%d files, %d dirs, %d bytes" % (stats["files"], stats["dirs"], stats["bytes"]))


if __name__ == "__main__":
    main()
