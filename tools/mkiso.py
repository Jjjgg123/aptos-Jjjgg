import argparse
import struct
import sys

SECTOR = 2048

FILES = {
    "/README.TXT": b"""aptos on a CD-ROM
=================

This disc was built by tools/mkiso.py, a ~200 line ISO9660 writer.
The kernel mounts it read-only through an ATAPI CD-ROM driver.

Mount it with:

  dev                 list block devices (cd0 shows up)
  mount cd0 /MNT/CD   mount the disc
  ls /MNT/CD          look around
  cat /MNT/CD/DOCS/NOTES.TXT

ISO9660 is read-only, so writes there fail - that is the point.
""",
    "/HELLO.TXT": b"Hello from a CD-ROM!\n",
    "/DOCS/NOTES.TXT": b"""Notes stored on the ISO9660 image.

  * sector size is 2048 bytes
  * directory records are variable length
  * names carry a ;1 version suffix
""",
    "/DOCS/SUB/DEEP.TXT": b"Nested two directories deep on the disc.\n",
}


def both32(v):
    return struct.pack("<I", v) + struct.pack(">I", v)


def both16(v):
    return struct.pack("<H", v) + struct.pack(">H", v)


def dirent(name, lba, size, is_dir):
    rec = bytearray()
    rec.append(0)
    rec.append(0)
    rec += both32(lba)
    rec += both32(size)
    rec += bytes([124, 1, 1, 0, 0, 0, 0])
    rec.append(2 if is_dir else 0)
    rec.append(0)
    rec.append(0)
    rec += both16(1)
    nb = name.encode("ascii")
    rec.append(len(nb))
    rec += nb
    if len(rec) % 2:
        rec.append(0)
    rec[0] = len(rec)
    return bytes(rec)


def build(tree, volume="APTOS_CD"):
    dirs = {"/": []}
    files = {}
    for path, content in tree.items():
        parts = [p for p in path.split("/") if p]
        cur = "/"
        for p in parts[:-1]:
            nxt = (cur.rstrip("/") + "/" + p)
            dirs.setdefault(nxt, [])
            if p not in dirs[cur]:
                dirs[cur].append(p)
            cur = nxt
        dirs[cur].append(parts[-1])
        files[path] = content

    order = ["/"]
    i = 0
    while i < len(order):
        d = order[i]
        for name in dirs[d]:
            sub = (d.rstrip("/") + "/" + name)
            if sub in dirs:
                order.append(sub)
        i += 1

    lba = 20
    dir_lba = {}
    dir_size = {}
    for d in order:
        entries = 0
        for name in dirs[d]:
            entries += 1
        for special in (".", ".."):
            entries += 1
        need = entries * 48 + 64
        sectors = max(1, (need + SECTOR - 1) // SECTOR)
        dir_lba[d] = lba
        dir_size[d] = sectors * SECTOR
        lba += sectors

    file_lba = {}
    file_size = {}
    for path, content in files.items():
        sectors = max(1, (len(content) + SECTOR - 1) // SECTOR)
        file_lba[path] = lba
        file_size[path] = len(content)
        lba += sectors

    total = lba
    img = bytearray(total * SECTOR)

    for d in order:
        buf = bytearray()
        parent = "/" if d == "/" else d.rsplit("/", 1)[0] or "/"
        buf += dirent("\x00", dir_lba[d], dir_size[d], True)
        buf += dirent("\x01", dir_lba[parent], dir_size[parent], True)
        for name in dirs[d]:
            sub = (d.rstrip("/") + "/" + name)
            if sub in dirs:
                buf += dirent(name, dir_lba[sub], dir_size[sub], True)
            else:
                path = sub
                buf += dirent(name + ";1", file_lba[path], file_size[path], False)
        img[dir_lba[d] * SECTOR: dir_lba[d] * SECTOR + len(buf)] = buf

    for path, content in files.items():
        off = file_lba[path] * SECTOR
        img[off:off + len(content)] = content

    pvd = bytearray(SECTOR)
    pvd[0] = 1
    pvd[1:6] = b"CD001"
    pvd[6] = 1
    pvd[8:40] = b" " * 32
    pvd[40:72] = volume.encode("ascii").ljust(32, b" ")
    pvd[80:88] = both32(total)
    pvd[120:124] = both16(1)
    pvd[124:128] = both16(1)
    pvd[128:132] = both16(SECTOR)
    pvd[132:140] = both32(0)
    pvd[140:144] = struct.pack("<I", 18)
    pvd[148:152] = struct.pack(">I", 19)
    pvd[156:156 + 34] = dirent("\x00", dir_lba["/"], dir_size["/"], True)
    pvd[190:318] = b" " * 128
    pvd[318:446] = b" " * 128
    pvd[446:574] = b" " * 128
    pvd[574:702] = b" " * 128
    pvd[702:739] = b" " * 37
    pvd[739:776] = b" " * 37
    pvd[776:813] = b" " * 37
    pvd[813:830] = bytes([124, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0])
    pvd[830:847] = bytes([124, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0])
    pvd[847:864] = b"0" * 16 + b"\x00"
    pvd[864:881] = b"0" * 16 + b"\x00"
    pvd[881] = 1
    img[16 * SECTOR:17 * SECTOR] = pvd

    term = bytearray(SECTOR)
    term[0] = 255
    term[1:6] = b"CD001"
    term[6] = 1
    img[17 * SECTOR:18 * SECTOR] = term

    pathrec = bytearray()
    pathrec.append(1)
    pathrec.append(0)
    pathrec += struct.pack("<I", dir_lba["/"])
    pathrec += struct.pack("<H", 1)
    pathrec += b"\x00"
    pathrec.append(0)
    img[18 * SECTOR:18 * SECTOR + len(pathrec)] = pathrec
    mrec = bytearray()
    mrec.append(1)
    mrec.append(0)
    mrec += struct.pack(">I", dir_lba["/"])
    mrec += struct.pack(">H", 1)
    mrec += b"\x00"
    mrec.append(0)
    img[19 * SECTOR:19 * SECTOR + len(mrec)] = mrec

    return bytes(img), total


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True)
    args = ap.parse_args()
    img, total = build(FILES)
    open(args.out, "wb").write(img)
    print("mkiso: %s (%d sectors, %d KiB)" % (args.out, total, total * 2))
    for path in sorted(FILES):
        print("    - %s (%d bytes)" % (path, len(FILES[path])))


if __name__ == "__main__":
    main()
