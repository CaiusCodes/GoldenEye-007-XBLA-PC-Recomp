#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
#
# Extracts files from Xbox 360 STFS (LIVE/PIRS/CON) containers.
#
# A Python port of tools/Extract-STFS.ps1 for macOS and Linux, where
# PowerShell is usually not installed. Same algorithm, same licence: the STFS
# block-offset and hash-table calculations are derived from Velocity
# (https://github.com/hetelek/Velocity), Copyright (C) the Velocity authors,
# licensed under the GNU General Public License version 3. The full licence
# text is in packaging/licenses/LICENSE-Extract-STFS.txt.
#
# Usage: python3 tools/extract_stfs.py <package> <output dir>
#        python3 tools/extract_stfs.py --list <package>

import os
import sys


def int24le(b):
    return b[0] | (b[1] << 8) | (b[2] << 16)


def int24be(b):
    return (b[0] << 16) | (b[1] << 8) | b[2]


def int32be(b):
    return int.from_bytes(b[:4], "big")


class Stfs:
    def __init__(self, f):
        self.f = f
        f.seek(0, os.SEEK_END)
        self.length = f.tell()
        f.seek(0)
        magic = f.read(4)
        if magic not in (b"CON ", b"LIVE", b"PIRS"):
            raise SystemExit(f"Not an STFS file (magic: {magic!r})")
        print(f"Package type: {magic.decode().strip()}")

        f.seek(0x340)
        header_size = int32be(f.read(4))
        self.first_hash_table = (header_size + 0xFFF) & 0xFFFFF000

        f.seek(0x379)
        f.read(2)  # volume descriptor size, reserved
        block_separation = f.read(1)[0]
        # Female (blockSep=1) -> shift 0, Male (blockSep=0) -> shift 1
        self.shift = 0 if block_separation & 1 else 1
        self.file_table_block_count = int.from_bytes(f.read(2), "little")
        self.file_table_block = int24le(f.read(3))

    def backing_data_block(self, n):
        s = self.shift
        r = (((n + 0xAA) // 0xAA) << s) + n
        if n < 0xAA:
            return r
        if n < 0x70E4:
            return r + (((n + 0x70E4) // 0x70E4) << s)
        return (1 << s) + (r + (((n + 0x70E4) // 0x70E4) << s))

    def block_to_address(self, n):
        return (self.backing_data_block(n) << 0xC) + self.first_hash_table

    def level0_hash_block(self, n):
        s = self.shift
        if n < 0xAA:
            return 0
        step0 = 0xAB if s == 0 else 0xAC
        num = (n // 0xAA) * step0
        num += ((n // 0x70E4) + 1) << s
        if n // 0x70E4 == 0:
            return num
        return num + (1 << s)

    def next_block(self, n):
        addr = (self.level0_hash_block(n) << 0xC) + self.first_hash_table
        self.f.seek(addr + (n % 0xAA) * 0x18 + 0x14)
        self.f.read(1)  # status
        nxt = int24be(self.f.read(3))
        if nxt >= 0xFFFFFF or nxt == 0xFFFFFE:
            return -1
        return nxt

    def entries(self):
        entries = []
        block = self.file_table_block
        for read in range(self.file_table_block_count):
            base = self.block_to_address(block)
            for i in range(64):
                off = base + i * 0x40
                if off + 0x40 > self.length:
                    break
                self.f.seek(off)
                e = self.f.read(0x40)
                name_len = e[0x28] & 0x3F
                if name_len == 0:
                    continue
                if any(c < 0x20 or c > 0x7E for c in e[:name_len]):
                    continue
                path_ind = (e[0x32] << 8) | e[0x33]
                entries.append({
                    "index": len(entries),
                    "name": e[:name_len].decode("ascii"),
                    "dir": bool(e[0x28] & 0x80),
                    "consecutive": bool(e[0x28] & 0x40),
                    "blocks": int24le(e[0x29:0x2C]),
                    "first": int24le(e[0x2F:0x32]),
                    "parent": -1 if path_ind == 0xFFFF else path_ind,
                    "size": int32be(e[0x34:0x38]),
                })
            if read + 1 < self.file_table_block_count:
                block = self.next_block(block)
                if block == -1:
                    break
        return entries


def relative_path(entry, dirs):
    parts = [entry["name"]]
    p = entry["parent"]
    while p != -1 and p in dirs:
        parts.insert(0, dirs[p]["name"])
        p = dirs[p]["parent"]
    return os.path.join(*parts)


def main(argv):
    list_only = "--list" in argv
    args = [a for a in argv if a != "--list"]
    if len(args) != (1 if list_only else 2):
        raise SystemExit(__doc__ or "usage: extract_stfs.py <package> <output dir>")
    with open(args[0], "rb") as f:
        stfs = Stfs(f)
        entries = stfs.entries()
        dirs = {e["index"]: e for e in entries if e["dir"]}
        print(f"Found {len(entries)} entries")
        out = None if list_only else args[1]
        if out:
            os.makedirs(out, exist_ok=True)
        for e in entries:
            rel = relative_path(e, dirs)
            if e["dir"]:
                print(f"  DIR:  {rel}")
                if out:
                    os.makedirs(os.path.join(out, rel), exist_ok=True)
                continue
            print(f"  FILE: {rel} ({e['size']} bytes)")
            if not out:
                continue
            dest = os.path.join(out, rel)
            os.makedirs(os.path.dirname(dest), exist_ok=True)
            with open(dest, "wb") as o:
                remaining, block, done = e["size"], e["first"], 0
                while remaining > 0 and done < e["blocks"]:
                    f.seek(stfs.block_to_address(block))
                    data = f.read(min(remaining, 0x1000))
                    if not data:
                        print(f"warning: read past end for {rel}", file=sys.stderr)
                        break
                    o.write(data)
                    remaining -= len(data)
                    done += 1
                    if remaining > 0:
                        if e["consecutive"]:
                            block += 1
                        else:
                            block = stfs.next_block(block)
                            if block == -1:
                                print(f"warning: block chain ended early for {rel}",
                                      file=sys.stderr)
                                break
        if out:
            print(f"\nExtraction complete to: {out}")


if __name__ == "__main__":
    main(sys.argv[1:])
