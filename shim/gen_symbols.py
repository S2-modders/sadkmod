#!/usr/bin/env python3
"""The crash report's names for the game's code: build/symbols.bin from the Ghidra map (../sourcemap).

Every function of SADK.exe and tincat3.dll with the ranges of its code, so that an address in a call stack becomes
"NNavy::Ship::dtor+0x23" (shim/symbols.cpp reads it). The file is a 4-byte little-endian size of the table, then
the table compressed with zlib:

    "SYM1", u32 module count
    per module (sadk::Module order: SADK.exe, tincat3.dll):
        u32 function count, u32 range count
        functions: u32 entry (static address), u32 name offset
        ranges, sorted by start: u32 start, u32 end (exclusive), u32 function index
    u32 size of the names, the names (NUL-terminated, "namespace::name")

    gen_symbols.py <sourcemap dir> <out file>
"""
import gzip
import json
import struct
import sys
import zlib

MODULES = ["sadk_noav.exe", "tincat3.dll"]


def main(src, out):
    names = bytearray()
    offsets = {}
    body = bytearray(b"SYM1" + struct.pack("<I", len(MODULES)))
    for program in MODULES:
        sm = json.load(gzip.open(f"{src}/{program}/sourcemap.json.gz"))
        funcs, ranges = [], []
        for f in sorted(sm["functions"], key=lambda f: int(f["entry"], 16)):
            name = (f["ns"] + "::" if f.get("ns") else "") + (f.get("name") or f"FUN_{f['entry']}")
            if name not in offsets:
                offsets[name] = len(names)
                names += name.encode("utf-8", "replace") + b"\0"
            idx = len(funcs)
            funcs.append((int(f["entry"], 16), offsets[name]))
            for start, end in f.get("body") or [[f["entry"], f["entry"]]]:
                s, e = int(start, 16), int(end, 16)
                ranges.append((s, max(e, s + 1), idx))
        ranges.sort()
        body += struct.pack("<II", len(funcs), len(ranges))
        for entry, off in funcs:
            body += struct.pack("<II", entry, off)
        for r in ranges:
            body += struct.pack("<III", *r)
    body += struct.pack("<I", len(names)) + names
    with open(out, "wb") as f:
        f.write(struct.pack("<I", len(body)) + zlib.compress(bytes(body), 9))


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
