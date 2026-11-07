import argparse
import struct
import sys

MAGIC = b"APTOSLST"
HEADER = 128
ENTRY = 64


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True)
    ap.add_argument("--name", default="aptos starter pack")
    ap.add_argument("--vendor", default="aptos")
    ap.add_argument("--description", default="installer + demo applications")
    ap.add_argument("--installer", default="installer")
    ap.add_argument("--app", action="append", default=[])
    args = ap.parse_args()

    apps = []
    for spec in args.app:
        name, _, path = spec.partition("=")
        if not path:
            raise SystemExit("mklst: --app wants name=path, got %r" % spec)
        apps.append((name, open(path, "rb").read()))
    if not apps:
        raise SystemExit("mklst: no applications given")

    installer = 0
    for i, (name, _) in enumerate(apps):
        if name == args.installer:
            installer = i
    if apps[installer][0] != args.installer:
        raise SystemExit("mklst: installer %r is not in the package" % args.installer)

    header = struct.pack(
        "<8sIIII32s32s24s",
        MAGIC, 1, len(apps), installer, 0,
        args.name.encode()[:32].ljust(32, b"\x00"),
        args.vendor.encode()[:32].ljust(32, b"\x00"),
        args.description.encode()[:24].ljust(24, b"\x00"),
    ).ljust(HEADER, b"\x00")

    offset = HEADER + len(apps) * ENTRY
    index = b""
    body = b""
    for name, data in apps:
        entry = struct.pack("<32sIII", name.encode()[:32].ljust(32, b"\x00"),
                            offset, len(data), 1).ljust(ENTRY, b"\x00")
        index += entry
        body += data
        offset += len(data)

    with open(args.out, "wb") as f:
        f.write(header + index + body)

    print("mklst: %s (%d apps, %d bytes)" % (args.out, len(apps),
                                             HEADER + len(index) + len(body)))
    for i, (name, data) in enumerate(apps):
        mark = "  <- installer" if i == installer else ""
        print("    %-14s %7d bytes%s" % (name, len(data), mark))


if __name__ == "__main__":
    main()
