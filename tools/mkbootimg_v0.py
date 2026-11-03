#!/usr/bin/env python3
"""mkbootimg minimal (en-tête Android v0), suffisant pour le vieux U-Boot NXP."""
import argparse, hashlib, struct

p = argparse.ArgumentParser()
p.add_argument("--kernel", required=True)
p.add_argument("--ramdisk", required=True)
p.add_argument("--second")
p.add_argument("--cmdline", default="")
p.add_argument("--base", type=lambda x: int(x, 0), default=0x14000000)
p.add_argument("--kernel_offset", type=lambda x: int(x, 0), default=0x00008000)
p.add_argument("--ramdisk_offset", type=lambda x: int(x, 0), default=0x01000000)
p.add_argument("--second_offset", type=lambda x: int(x, 0), default=0x00f00000)
p.add_argument("--tags_offset", type=lambda x: int(x, 0), default=0x00000100)
p.add_argument("--pagesize", type=int, default=2048)
p.add_argument("-o", "--output", required=True)
a = p.parse_args()

k = open(a.kernel, "rb").read()
r = open(a.ramdisk, "rb").read()
s = open(a.second, "rb").read() if a.second else b""

sha = hashlib.sha1()
for blob in (k, r, s):
    sha.update(blob)
    sha.update(struct.pack("<I", len(blob)))
ident = sha.digest().ljust(32, b"\0")

cmd = a.cmdline.encode()
assert len(cmd) < 512
hdr = b"ANDROID!" + struct.pack(
    "<10I",
    len(k), a.base + a.kernel_offset,
    len(r), a.base + a.ramdisk_offset,
    len(s), a.base + a.second_offset,
    a.base + a.tags_offset, a.pagesize, 0, 0,
) + b"maxwell-dump".ljust(16, b"\0") + cmd.ljust(512, b"\0") + ident + b"\0" * 1024

def pad(b):
    return b + b"\0" * ((-len(b)) % a.pagesize)

with open(a.output, "wb") as f:
    f.write(pad(hdr))
    f.write(pad(k))
    f.write(pad(r))
    if s:
        f.write(pad(s))
print(f"{a.output}: kernel {len(k)} o @0x{a.base + a.kernel_offset:08x}, "
      f"ramdisk {len(r)} o, second {len(s)} o")
