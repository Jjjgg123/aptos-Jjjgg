#!/usr/bin/env python3
import argparse
import struct
import sys

SECTOR = 2048
SYSTEM_AREA = 16
BOOT_RECORD_SECTOR = 17
TERMINATOR_SECTOR = 18
BOOT_CATALOG_SECTOR = 19
PATH_L_SECTOR = 20
PATH_M_SECTOR = 21
FIRST_DATA_SECTOR = 22

MEDIA_NO_EMU = 0
MEDIA_FLOPPY_144 = 2
MEDIA_FLOPPY_288 = 3
MEDIA_HDD = 4

FLOPPY_144_BYTES = 1474560
FLOPPY_288_BYTES = 2949120


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


def build_iso(files, boot_image, media, out, volume="APTOS_CD", boot_label="aptos setup"):
    dirs = {"/": []}
    for path in files:
        parts = [p for p in path.split("/") if p]
        cur = "/"
        for p in parts[:-1]:
            nxt = cur.rstrip("/") + "/" + p
            dirs.setdefault(nxt, [])
            if p not in dirs[cur]:
                dirs[cur].append(p)
            cur = nxt
        dirs[cur].append(parts[-1])

    order = ["/"]
    i = 0
    while i < len(order):
        d = order[i]
        for name in dirs[d]:
            sub = d.rstrip("/") + "/" + name
            if sub in dirs:
                order.append(sub)
        i += 1

    lba = FIRST_DATA_SECTOR
    dir_lba, dir_size = {}, {}
    for d in order:
        entries = len(dirs[d]) + 2
        need = entries * 48 + 64
        sectors = max(1, (need + SECTOR - 1) // SECTOR)
        dir_lba[d] = lba
        dir_size[d] = sectors * SECTOR
        lba += sectors

    file_lba, file_size = {}, {}
    for path in sorted(files, key=lambda p: (p.count("/"), p)):
        content = files[path]
        sectors = max(1, (len(content) + SECTOR - 1) // SECTOR)
        file_lba[path] = lba
        file_size[path] = len(content)
        lba += sectors

    boot_lba = lba
    boot_sectors = (len(boot_image) + SECTOR - 1) // SECTOR
    lba += boot_sectors
    total = lba

    img = bytearray(total * SECTOR)

    for d in order:
        buf = bytearray()
        parent = "/" if d == "/" else (d.rsplit("/", 1)[0] or "/")
        buf += dirent("\x00", dir_lba[d], dir_size[d], True)
        buf += dirent("\x01", dir_lba[parent], dir_size[parent], True)
        for name in dirs[d]:
            sub = d.rstrip("/") + "/" + name
            if sub in dirs:
                buf += dirent(name, dir_lba[sub], dir_size[sub], True)
            else:
                buf += dirent(name + ";1", file_lba[sub], file_size[sub], False)
        img[dir_lba[d] * SECTOR: dir_lba[d] * SECTOR + len(buf)] = buf

    for path, content in files.items():
        off = file_lba[path] * SECTOR
        img[off:off + len(content)] = content

    img[boot_lba * SECTOR: boot_lba * SECTOR + len(boot_image)] = boot_image

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
    pvd[140:144] = struct.pack("<I", PATH_L_SECTOR)
    pvd[148:152] = struct.pack(">I", PATH_M_SECTOR)
    pvd[156:190] = dirent("\x00", dir_lba["/"], dir_size["/"], True)
    pvd[190:318] = b" " * 128
    pvd[318:446] = b" " * 128
    pvd[446:574] = b" " * 128
    pvd[574:702] = b" " * 128
    pvd[702:739] = b" " * 37
    pvd[739:776] = b" " * 37
    pvd[776:813] = b" " * 37
    for off in (813, 830):
        pvd[off:off + 17] = bytes([124, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0])
    pvd[847:864] = b"0" * 16 + b"\x00"
    pvd[864:881] = b"0" * 16 + b"\x00"
    pvd[881] = 1
    img[16 * SECTOR:17 * SECTOR] = pvd

    brvd = bytearray(SECTOR)
    brvd[0] = 0
    brvd[1:6] = b"CD001"
    brvd[6] = 1
    brvd[7:39] = b"EL TORITO SPECIFICATION".ljust(32, b"\x00")
    brvd[39:71] = b"\x00" * 32
    brvd[71:75] = struct.pack("<I", BOOT_CATALOG_SECTOR)
    img[BOOT_RECORD_SECTOR * SECTOR:(BOOT_RECORD_SECTOR + 1) * SECTOR] = brvd

    term = bytearray(SECTOR)
    term[0] = 255
    term[1:6] = b"CD001"
    term[6] = 1
    img[TERMINATOR_SECTOR * SECTOR:(TERMINATOR_SECTOR + 1) * SECTOR] = term

    pathrec = bytearray()
    pathrec.append(1)
    pathrec.append(0)
    pathrec += struct.pack("<I", dir_lba["/"])
    pathrec += struct.pack("<H", 1)
    pathrec += b"\x00"
    pathrec.append(0)
    img[PATH_L_SECTOR * SECTOR:PATH_L_SECTOR * SECTOR + len(pathrec)] = pathrec
    mrec = bytearray()
    mrec.append(1)
    mrec.append(0)
    mrec += struct.pack(">I", dir_lba["/"])
    mrec += struct.pack(">H", 1)
    mrec += b"\x00"
    mrec.append(0)
    img[PATH_M_SECTOR * SECTOR:PATH_M_SECTOR * SECTOR + len(mrec)] = mrec

    if media == MEDIA_HDD:
        sector_count = boot_sectors
    elif media == MEDIA_NO_EMU:
        sector_count = 4
    else:
        sector_count = 1

    cat = bytearray(SECTOR)
    val = bytearray(32)
    val[0] = 1
    val[1] = 0
    val[4:28] = boot_label.encode("ascii")[:24].ljust(24, b"\x00")
    val[30] = 0x55
    val[31] = 0xAA
    checksum = (-sum(struct.unpack("<16H", val))) & 0xFFFF
    val[28:30] = struct.pack("<H", checksum)
    cat[0:32] = val

    ent = bytearray(32)
    ent[0] = 0x88
    ent[1] = media
    ent[2:4] = struct.pack("<H", 0)
    ent[4] = 0
    ent[6:8] = struct.pack("<H", sector_count)
    ent[8:12] = struct.pack("<I", boot_lba)
    cat[32:64] = ent
    img[BOOT_CATALOG_SECTOR * SECTOR:(BOOT_CATALOG_SECTOR + 1) * SECTOR] = cat

    with open(out, "wb") as f:
        f.write(img)

    names = {MEDIA_NO_EMU: "no-emulation", MEDIA_FLOPPY_144: "1.44MB floppy",
             MEDIA_FLOPPY_288: "2.88MB floppy", MEDIA_HDD: "hard disk"}
    print("mkcd: %s (%d sectors, %.1f MiB)" % (out, total, total * SECTOR / 1048576.0))
    print("      boot image : LBA %d, %d bytes (%s emulation)"
          % (boot_lba, len(boot_image), names.get(media, "?")))
    print("      catalog    : LBA %d    volume: %s" % (BOOT_CATALOG_SECTOR, volume))
    for path in sorted(files):
        print("      %-22s %8d bytes @ LBA %d" % (path, file_size[path], file_lba[path]))
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True)
    ap.add_argument("--boot", required=True)
    ap.add_argument("--media", default="floppy144",
                    choices=["floppy144", "floppy288", "hdd", "none"])
    ap.add_argument("--volume", default="APTOS_CD")
    ap.add_argument("--file", action="append", default=[])
    args = ap.parse_args()

    boot = open(args.boot, "rb").read()
    if args.media == "floppy144":
        media = MEDIA_FLOPPY_144
        if len(boot) != FLOPPY_144_BYTES:
            raise SystemExit("mkcd: 1.44MB floppy emulation needs exactly %d bytes"
                             % FLOPPY_144_BYTES)
    elif args.media == "floppy288":
        media = MEDIA_FLOPPY_288
        if len(boot) != FLOPPY_288_BYTES:
            raise SystemExit("mkcd: 2.88MB floppy emulation needs exactly %d bytes"
                             % FLOPPY_288_BYTES)
    elif args.media == "hdd":
        media = MEDIA_HDD
    else:
        media = MEDIA_NO_EMU
        if len(boot) > SECTOR:
            raise SystemExit("mkcd: no-emulation boot image must be one 2048-byte sector")

    files = {}
    for spec in args.file:
        name, _, path = spec.partition("=")
        files[name] = open(path, "rb").read()

    return build_iso(files, boot, media, args.out, args.volume)


if __name__ == "__main__":
    sys.exit(main())
