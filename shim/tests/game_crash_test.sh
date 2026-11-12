#!/bin/sh
# The crash report in the real game under Wine (Makefile: make test-game GAME=<install>): SADK.exe (the DRM-free
# build) with this shim and the crashtest mod (game/crashtest), crashed early in its start-up in each way crashtest.cpp
# knows; game_crash_check.py checks the reports. The install is not changed: the test runs in a folder of links to
# it ($WORK, default ~/scratch/sadk-crashtest), deleted afterwards. Needs Wine and xvfb-run (the game's window).
set -e
game=$(cd "$1" && pwd)
here=$(cd "$(dirname "$0")" && pwd)
work=${WORK:-$HOME/scratch/sadk-crashtest}
export WINEDEBUG=-all WINEDLLOVERRIDES="wsock32=n,b"
# the game's dump folder before it sets its own: <My Documents>\SAdK\dumps of the Wine user
docs=${WINEPREFIX:-$HOME/.wine}/drive_c/users/$USER/Documents/SAdK/dumps
fail=0
for s in survive mod native called thread abort purecall; do
    rm -rf "$work"
    mkdir -p "$work/bin" "$work/mods/crashtest"
    for f in "$game"/*; do [ "$(basename "$f")" = bin ] || ln -s "$f" "$work/"; done
    for f in "$game"/bin/*; do ln -s "$f" "$work/bin/"; done
    rm -f "$work/bin/wsock32.dll" "$work/bin/WSOCK32.dll" "$work/bin/wsock32_shim.txt"
    cp "$here/../wsock32.dll" "$work/bin/wsock32.dll"
    [ -e "$work/bin/PhysXLoader.dll" ] || cp "$here/game/build/PhysXLoader.dll" "$work/bin/"
    cp "$here/game/build/crashtest/mod.dll" "$work/mods/crashtest/"
    printf '[crash]\nkind = %s\n' "$s" > "$work/mods/crashtest/crashtest.ini"
    before=$(ls "$docs" 2>/dev/null | sort)
    # abort and purecall end in the C runtime's message box after the report: the timeout ends them. Now and then
    # Wine does not get the game's window up and the game ends before the hook: then once more.
    for try in 1 2; do
        rm -f "$work/bin/wsock32_shim.txt"
        timeout 90 xvfb-run -a wine "$work/bin/SADK.exe" > "$work/stdout.txt" 2>&1 || true
        wineserver -k 2>/dev/null || true   # the next run starts its own Wine session (with its own display)
        wineserver -w 2>/dev/null || true
        grep -q "crashtest: $s" "$work/bin/wsock32_shim.txt" 2>/dev/null && break
        echo "  $s: the game did not reach the hook (try $try)"
    done
    python3 -I "$here/game_crash_check.py" "$work" "$docs" "$s" "$before" || fail=1
    for f in $(ls "$docs" 2>/dev/null); do   # the files this run made in the Wine user's dump folder
        echo "$before" | grep -qx "$f" || { mkdir -p "$work.reports/$s"; mv "$docs/$f" "$work.reports/$s/"; }
    done
done
rm -rf "$work"
[ $fail = 0 ] && echo "crash report in the game: all scenarios passed (reports in $work.reports)"
exit $fail
