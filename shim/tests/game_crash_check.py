#!/usr/bin/env python3
"""Checks one scenario of game_crash_test.sh: game_crash_check.py <work> <dump folder> <scenario> <files before>."""
import os
import sys
import zipfile

work, docs, scenario, before = sys.argv[1], sys.argv[2], sys.argv[3], set(sys.argv[4].split())
new = sorted(f for f in (os.listdir(docs) if os.path.isdir(docs) else []) if f not in before)
shim_log = open(os.path.join(work, "bin", "wsock32_shim.txt"), encoding="latin-1").read()
failures = []


def has(text, needle, where):
    if needle not in text:
        failures.append(f"{where} lacks {needle!r}")


zips = [f for f in new if f.startswith("crash_") and f.endswith(".zip")]
errors = [f for f in new if f.startswith("errors_")]
text = ""
if scenario == "survive":
    if zips or len(errors) != 1:
        failures.append(f"expected one errors file and no crash: {new}")
    else:
        text = open(os.path.join(docs, errors[0]), encoding="latin-1").read()
        has(text, 'Cause:       mod "crashtest"', "errors file")
        has(text, "mods\\crashtest\\mod.dll+0x", "errors file")
        has(text, "2 times", "errors file summary")
else:
    if len(zips) != 1 or errors:
        failures.append(f"expected one crash ZIP (with the errors inside): {new}")
    else:
        z = zipfile.ZipFile(os.path.join(docs, zips[0]))
        names = z.namelist()
        log = [n for n in names if n.startswith("crashlog_")]
        dmp = [n for n in names if n.startswith("crashdump_")]
        if len(log) != 1 or len(dmp) != 1 or "wsock32_shim.txt" not in names:
            failures.append(f"ZIP content: {names}")
        if dmp:
            info = z.getinfo(dmp[0])
            print(f"  {scenario}: dump {info.file_size >> 20} MB, packed {info.compress_size >> 20} MB")
        if log:
            text = z.read(log[0]).decode("latin-1")
            expected = {
                "mod": ('Cause:       mod "crashtest"', "the game's crash handler (WinMain)"),
                "native": ("Cause:       the game itself", "the game's crash handler (WinMain)"),
                "called": ('Cause:       probably mod "crashtest"', "Generic_StdString_cstr_at4+0x"),
                "thread": ('Cause:       mod "crashtest"', "Windows' last-chance filter"),
                "abort": ('Cause:       mod "crashtest"', "abort() was called"),
                "purecall": ('Cause:       mod "crashtest"', "a pure virtual function was called"),
            }[scenario]
            for e in expected:
                has(text, e, "crash log")
            has(text, "Game:        SADK.exe build 34688, no match running", "crash log")
            has(text, "hook  ", "crash log (changes to the game's code)")
            if scenario in ("mod", "native", "called"):
                has(text, "CApplicationEx::Initialize+0x", "crash log (the call stack with names)")
                has(text, "WinMain+0x", "crash log (the call stack with names)")
            has(shim_log, "crash report: ", "the shim's log")
            has(text, "3  crashtest: level 3, text, 1.50, wide, (bad pointer)  (2 times", "crash log (the game's log)")

if failures:
    print(f"crash report in the game, scenario {scenario}: FAILED")
    for f in failures:
        print("  - " + f)
    print(text[:4000])
    print("--- shim log (end)\n" + shim_log[-3000:])
    sys.exit(1)
print(f"crash report in the game, scenario {scenario}: ok")
