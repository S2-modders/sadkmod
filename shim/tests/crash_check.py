#!/usr/bin/env python3
"""Checks what one scenario of crash_test.cpp left in <root>/dumps (crash_test.sh)."""
import glob
import os
import sys
import zipfile

root, scenario = sys.argv[1], sys.argv[2]
dumps = os.path.join(root, "dumps")
files = sorted(os.listdir(dumps)) if os.path.isdir(dumps) else []
failures = []


def expect(ok, what):
    if not ok:
        failures.append(what)


def has(text, needle, where):
    expect(needle in text, f"{where} lacks {needle!r}")


zips = [f for f in files if f.startswith("crash_") and f.endswith(".zip")]
errors = [f for f in files if f.startswith("errors_")]
loose = [f for f in files if f.startswith(("crashlog_", "crashdump_"))]
expect(not loose, f"loose report files left: {loose}")

if scenario == "clean":
    expect(not zips, f"a crash report after a clean run: {zips}")
    expect(len(errors) == 1, f"one errors file expected: {files}")
    if errors:
        text = open(os.path.join(dumps, errors[0]), encoding="latin-1").read()
        has(text, 'Cause:       mod "crashmod"', "errors file")
        has(text, "mods\\crashmod\\mod.dll+0x", "errors file")
        has(text, "2 times", "errors file summary")
        expect(text.count("Cause:") == 1, "errors file: the repeat was written out again")
else:
    expect(len(zips) == 1, f"one crash ZIP expected: {files}")
    expect(not errors, f"the errors file should be in the ZIP: {errors}")
    if zips:
        z = zipfile.ZipFile(os.path.join(dumps, zips[0]))
        names = z.namelist()
        log = [n for n in names if n.startswith("crashlog_")]
        dmp = [n for n in names if n.startswith("crashdump_")]
        expect(len(log) == 1 and len(dmp) == 1, f"ZIP lacks the log or the dump: {names}")
        expect("wsock32_shim.txt" in names, f"ZIP lacks the shim log: {names}")
        if dmp:
            expect(z.read(dmp[0])[:4] == b"MDMP", "the dump is not a minidump")
            info = z.getinfo(dmp[0])
            print(f"  {scenario}: dump {info.file_size >> 20} MB, packed {info.compress_size >> 20} MB")
        if log:
            text = z.read(log[0]).decode("latin-1")
            cause = {"mod": 'Cause:       mod "crashmod"', "native": "Cause:       the game itself",
                     "called": 'Cause:       probably mod "crashmod"', "thread": 'Cause:       mod "crashmod"'}[scenario]
            has(text, cause, "crash log")
            has(text, "EXCEPTION_ACCESS_VIOLATION (0xC0000005): writing address 0x00000000", "crash log")
            has(text, "Call stack", "crash log")
            has(text, "Dump: dump type", "crash log")
            if scenario == "thread":
                has(text, "Windows' last-chance filter", "crash log")
            if scenario == "mod":
                expect(any(n.startswith("errors_") for n in names), f"ZIP lacks the errors file: {names}")
                has(text, "Errors the game survived before, this session: 1", "crash log")
                errs = [n for n in names if n.startswith("errors_")]
                if errs:
                    has(z.read(errs[0]).decode("latin-1"), "2 times", "errors file in the ZIP")
            if failures:
                print(text[:3000])

if failures:
    print(f"crash report, scenario {scenario}: FAILED")
    for f in failures:
        print("  - " + f)
    print("  files:", files)
    sys.exit(1)
print(f"crash report, scenario {scenario}: ok")
