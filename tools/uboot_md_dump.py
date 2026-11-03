#!/usr/bin/env python3
"""
Dump d'une zone de RAM par la console U-Boot (commande md.b), avec reprise et contrôle CRC32.

Usage typique (dans U-Boot, préparer d'abord la zone en RAM, ex. `mmc read` ou `sf read`) :
    python3 uboot_md_dump.py --port /dev/ttyUSB0 --addr 0x12000000 --size 0x200000 -o nor.bin

- lecture par blocs (md.b), vérification de la continuité des adresses, nouvel essai si un bloc est corrompu ;
- reprise : si le fichier de sortie existe, on repart de sa taille ;
- à la fin, `crc32 <addr> <size>` côté U-Boot est comparé au CRC32 local.

Uniquement des lectures : n'envoie que md.b et crc32.
Débit attendu à 115200 bauds : ~2,5 Ko/s (≈ 15 min pour 2 Mo).
"""
import argparse
import os
import re
import sys
import time
import zlib

import serial  # pip install pyserial

LINE = re.compile(r"^([0-9a-fA-F]{8}):((?: [0-9a-fA-F]{2}){1,16})")


def run(ser, cmd, timeout):
    ser.reset_input_buffer()
    ser.write(cmd.encode() + b"\r")
    buf = b""
    deadline = time.time() + timeout
    while time.time() < deadline:
        chunk = ser.read(ser.in_waiting or 1)
        if chunk:
            buf += chunk
            deadline = time.time() + timeout
            if buf.rstrip().endswith(b"U-Boot >"):
                return buf.decode(errors="replace")
    raise TimeoutError(f"pas de prompt après {cmd!r}")


def parse_md(text, addr, n):
    out = bytearray()
    for line in text.splitlines():
        m = LINE.match(line.strip())
        if not m:
            continue
        a = int(m.group(1), 16)
        if a != addr + len(out):
            raise ValueError(f"adresse inattendue 0x{a:08x} (attendu 0x{addr + len(out):08x})")
        out += bytes(int(x, 16) for x in m.group(2).split())
    if len(out) != n:
        raise ValueError(f"{len(out)} octets reçus au lieu de {n}")
    return bytes(out)


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--port", required=True)
    p.add_argument("--baud", type=int, default=115200)
    p.add_argument("--addr", type=lambda x: int(x, 0), required=True)
    p.add_argument("--size", type=lambda x: int(x, 0), required=True)
    p.add_argument("--chunk", type=lambda x: int(x, 0), default=0x1000)
    p.add_argument("-o", "--output", required=True)
    a = p.parse_args()

    ser = serial.Serial(a.port, a.baud, timeout=0.2)
    run(ser, "", 3)  # resynchronise sur le prompt

    done = os.path.getsize(a.output) if os.path.exists(a.output) else 0
    done -= done % 16
    with open(a.output, "r+b" if done else "wb") as f:
        f.truncate(done)
        f.seek(done)
        t0 = time.time()
        start = done
        while done < a.size:
            n = min(a.chunk, a.size - done)
            for essai in range(5):
                try:
                    data = parse_md(run(ser, f"md.b {a.addr + done:x} {n:x}", 5), a.addr + done, n)
                    break
                except (ValueError, TimeoutError) as e:
                    print(f"\nbloc 0x{done:x} : {e}, nouvel essai", file=sys.stderr)
                    run(ser, "", 3)
            else:
                sys.exit("abandon : bloc illisible après 5 essais (relancer pour reprendre)")
            f.write(data)
            f.flush()
            done += n
            rate = (done - start) / max(time.time() - t0, 0.001)
            eta = (a.size - done) / max(rate, 1)
            print(f"\r{done:#x}/{a.size:#x}  {rate / 1024:.1f} Ko/s  reste {eta / 60:.0f} min ",
                  end="", flush=True)
    print()

    local = zlib.crc32(open(a.output, "rb").read()) & 0xFFFFFFFF
    reply = run(ser, f"crc32 {a.addr:x} {a.size:x}", 60)
    m = re.search(r"==>\s*([0-9a-fA-F]{8})", reply)
    if not m:
        print(f"CRC local {local:08x} ; réponse U-Boot non reconnue :\n{reply}")
        return
    remote = int(m.group(1), 16)
    print(f"CRC32 local {local:08x}, U-Boot {remote:08x} : {'OK' if local == remote else 'DIFFÉRENT'}")
    sys.exit(0 if local == remote else 1)


if __name__ == "__main__":
    main()
