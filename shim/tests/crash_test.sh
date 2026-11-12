#!/bin/sh
# The crash report outside the game under Wine (Makefile: make test). Each scenario of crash_test.cpp runs in a fresh
# folder build/crashroot (bin\, mods\crashmod\, dumps\); crash_check.py checks what it left.
set -e
cd "$(dirname "$0")/../build"
for s in clean mod native called thread; do
    rm -rf crashroot
    mkdir -p crashroot/bin crashroot/mods/crashmod
    cp crash_test.exe crashroot/bin/
    cp crash_mod.dll crashroot/mods/crashmod/mod.dll
    WINEDEBUG=-all timeout 300 wine crashroot/bin/crash_test.exe $s > crashroot/stdout.txt 2>&1 || true
    python3 -I ../tests/crash_check.py crashroot $s
done
echo "crash report: all scenarios passed"
