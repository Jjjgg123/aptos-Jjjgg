#!/usr/bin/env python3
import argparse
import math
import struct
import sys

SECTOR = 512
ENTRY = 64
KERNEL_LBA = 9

MIN_MATCH = 3
MAX_MATCH = 18
WINDOW = 4095

HEADER = 24


def lzss_compress(data):
    out = bytearray()
    pend = bytearray()
    flags = 0
    nbits = 0
    pos = {}
    i = 0
    n = len(data)

    while i < n:
        best_len = 0
        best_off = 0

        if i + MIN_MATCH <= n:
            key = bytes(data[i:i + MIN_MATCH])
            j = pos.get(key, -1)
            if j >= 0 and 0 < i - j <= WINDOW:
                l = MIN_MATCH
                while (l < MAX_MATCH and i + l < n and data[j + l] == data[i + l]):
                    l += 1
                best_len = l
                best_off = i - j
                pos[key] = i
                for k in range(1, best_len):
                    if i + k + MIN_MATCH <= n:
                        pos[bytes(data[i + k:i + k + MIN_MATCH])] = i + k

        if best_len >= MIN_MATCH:
            off = best_off
            ln = best_len - MIN_MATCH
            pend.append(off & 0xFF)
            pend.append(((ln & 0x0F) << 4) | ((off >> 8) & 0x0F))
            i += best_len
        else:
            if i + MIN_MATCH <= n:
                pos[bytes(data[i:i + MIN_MATCH])] = i
            pend.append(data[i])
            flags |= (1 << nbits)
            i += 1

        nbits += 1
        if nbits == 8:
            out.append(flags)
            out += pend
            pend = bytearray()
            flags = 0
            nbits = 0

    if nbits:
        out.append(flags)
        out += pend
    return bytes(out)


def lzss_decompress(src, dlen):
    dst = bytearray()
    si = 0
    flags = 0
    bits = 0
    slen = len(src)
    while si < slen and len(dst) < dlen:
        if bits == 0:
            flags = src[si]
            si += 1
            bits = 8
        if flags & 1:
            dst.append(src[si])
            si += 1
        else:
            b0 = src[si]
            b1 = src[si + 1]
            si += 2
            off = b0 | ((b1 & 0x0F) << 8)
            ln = (b1 >> 4) + MIN_MATCH
            p = len(dst) - off
            if p < 0:
                raise SystemExit("mkert: corrupt stream (offset %d at %d)" % (off, len(dst)))
            for _ in range(ln):
                dst.append(dst[p])
                p += 1
        flags >>= 1
        bits -= 1
    return bytes(dst)


def find_fs(d):
    off = d.find(b"MYO2")
    if off < 0:
        raise SystemExit("mkert: no MYO2 block found in the image")
    kernel_lba, kernel_sec, fs_lba = struct.unpack_from("<IHI", d, off + 4)
    if d[fs_lba * SECTOR:fs_lba * SECTOR + 8] != b"MINIFS02":
        raise SystemExit("mkert: no MINIFS02 at sector %d" % fs_lba)
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
        })
    return out


def walk(d, start, entries, prefix, acc, excl):
    for e in read_dir(d, start, entries):
        path = prefix + e["name"]
        if any(path.startswith(x) for x in excl):
            continue
        if e["dir"]:
            acc.append((1, path, b""))
            walk(d, e["first"], e["size"], path + "/", acc, excl)
        else:
            data = bytes(d[e["first"] * SECTOR:e["first"] * SECTOR + e["size"]])
            acc.append((0, path, data))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True)
    ap.add_argument("--disk", required=True, help="source image to snapshot")
    ap.add_argument("--kernel", required=True, help="kernel flat binary")
    ap.add_argument("--name", default="aptos")
    ap.add_argument("--exclude", default="", help="comma-separated path prefixes to skip")
    args = ap.parse_args()

    d = open(args.disk, "rb").read()
    kernel = open(args.kernel, "rb").read()
    fs_lba = find_fs(d)

    blob_sectors = KERNEL_LBA + math.ceil(len(kernel) / SECTOR)
    if blob_sectors != fs_lba:
        raise SystemExit("mkert: blob ends at sector %d but the filesystem starts at %d"
                         % (blob_sectors, fs_lba))
    blob = bytes(d[:blob_sectors * SECTOR])

    sb = struct.unpack_from("<8sIIIIIIIIII32s", d, fs_lba * SECTOR)
    dir_start, max_entries = sb[5], sb[9]

    tree = []
    excl = [x.strip() for x in args.exclude.split(",") if x.strip()]
    walk(d, dir_start, max_entries, "/", tree, excl)

    blob_c = lzss_compress(blob)
    if lzss_decompress(blob_c, len(blob)) != blob:
        raise SystemExit("mkert: boot blob round-trip failed")

    body = bytearray()
    total_raw = 0
    total_cmp = 0
    for kind, path, data in tree:
        name = path.lstrip("/").encode("ascii")
        comp = b"" if kind == 1 else lzss_compress(data)
        if kind == 0:
            if lzss_decompress(comp, len(data)) != data:
                raise SystemExit("mkert: round-trip failed for %s" % path)
            total_raw += len(data)
            total_cmp += len(comp)
        body += struct.pack("<BBHII", kind, 0, len(name), len(data), len(comp))
        body += name
        body += comp

    header = struct.pack("<4sIIIII", b"ERT1", 1, len(blob), len(blob_c), len(tree), 0)
    assert len(header) == HEADER
    out = header + blob_c + bytes(body)
    open(args.out, "wb").write(out)

    print("mkert: %s  (%.1f KiB)" % (args.out, len(out) / 1024.0))
    print("       boot blob  %6d bytes -> %6d compressed (%.0f%%)"
          % (len(blob), len(blob_c), 100.0 * len(blob_c) / len(blob)))
    print("       entries    %6d  (%d dirs)" % (len(tree),
                                                sum(1 for k, _, _ in tree if k == 1)))
    print("       file data  %6d bytes -> %6d compressed (%.0f%%)"
          % (total_raw, total_cmp, 100.0 * total_cmp / max(1, total_raw)))
    print("       filesystem starts at sector %d" % fs_lba)
    for kind, path, data in tree:
        print("         %s %-24s %7d" % ("d" if kind == 1 else "-", path, len(data)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
