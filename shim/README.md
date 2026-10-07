# wsock32 test shim

A proxy `wsock32.dll` for the game's networking DLL. `tincat3.dll` imports all of its sockets from
`WSOCK32.dll` by ordinal; `SADK.exe` uses `WS2_32.dll` directly, so this shim only sees TinCat traffic.
`WSOCK32.dll` is not a KnownDLL, so Windows loads a copy from the game folder before the system one.

Load test passed 2026-10-07 on the maintainer's Windows PC (loaded from the game folder, all exports resolved).

This build is a load test only: it forwards every export by ordinal to the system `wsock32.dll`, writes
`wsock32_shim.txt` next to the game exe when it is loaded, and appends one line per `listen()` call with
the local port (the host's match server opens its game port with it).

Build (Linux, mingw-w64): `make` -> `wsock32.dll`. Install: copy it next to `SADK.exe`. Remove it to undo.
