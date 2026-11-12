#!/bin/sh
# The crash report outside the game under Wine (Makefile: make test). Each scenario of crash_test.cpp runs in a fresh
# folder build/crashroot (bin\, mods\crashmod\, dumps\); crash_check.py checks what it left.
set -e
cd "$(dirname "$0")/../build"
# old-*: the same with a mod built without frame pointers (before sadkmod kept them)
for s in clean mod native called thread old-mod old-called; do
    rm -rf crashroot
    mkdir -p crashroot/bin crashroot/mods/crashmod
    cp crash_test.exe crashroot/bin/
    case $s in old-*) cp crash_mod_old.dll crashroot/mods/crashmod/mod.dll ;; *) cp crash_mod.dll crashroot/mods/crashmod/mod.dll ;; esac
    WINEDEBUG=-all timeout 300 wine crashroot/bin/crash_test.exe ${s#old-} > crashroot/stdout.txt 2>&1 || true
    python3 -I ../tests/crash_check.py crashroot ${s#old-}
done
echo "crash report: all scenarios passed"
