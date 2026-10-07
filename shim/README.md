# wsock32 bridge shim

A proxy `wsock32.dll` that lets a host behind NAT be joined through the lobby server. Protocol and the
evidence behind it: `docs/bridge-protocol.md`; the server side is `sadk_lobby/bridge.py`.

`tincat3.dll` imports all of its sockets from `WSOCK32.dll` by ordinal; `SADK.exe` uses `WS2_32.dll`
directly, so the shim only sees TinCat traffic. `WSOCK32.dll` is not a KnownDLL, so Windows loads a copy
from the game folder before the system one (live 2026-10-07: loaded, all exports resolved).

Every export is forwarded by ordinal to the system `wsock32.dll`; `connect`, `send`, `listen` and
`closesocket` are hooked. It also enables map sharing (missing maps download from the host, custom maps
in `Documents\SAdK\maps` appear in the map picker) through in-memory patches to `SADK.exe`, listed in
`docs/BINARY_PATCHES.md`, and it is the **mod host**: it loads `<game>\mods\*` (data overrides and `mod.dll`
files, `mods/README.md`). The shim logs to `wsock32_shim.txt` next to the game exe; mods log there too.

Start-up: DllMain only loads the real `wsock32.dll` and puts a jump on `SADK.exe`'s entry point (the shim is
loaded with the process: `SADK.exe` imports `tincat3.dll`, which imports `WSOCK32`). The jump runs once on the
main thread before any game code: it restores the entry point, checks the exe, applies the map-sharing patches,
loads the mods and starts the bridge thread, then calls the real entry point.

- **Requires the DRM-free `SADK.exe`** (MD5 `d4832bc5103c14f5445471af29b8d778`). The shim hashes the exe at
  start-up and stays a pure pass-through on any other build: no bridge, no lobby tag, no game patches.
- Source: C++23 on `sadkmod/` (the repo's modding library): `main.cpp` (load, forwarding), `config.cpp`,
  `bridge.cpp` (the bridge and the hooked socket functions), `mapshare.cpp`. The mod host itself is in sadkmod
  (`sadkmod/src/host.cpp`).
- Build (Linux, mingw-w64, Python 3): `make` -> `wsock32.dll` (builds `sadkmod` first).
- `make verify` checks every patch's expected bytes against a DRM-free `SADK.exe` under Wine, without running
  the game (`SADK_EXE=<path>`, default `~/sadk_game/bin/SADK.exe`).
- Install: copy it into the game's `bin` folder, next to `SADK.exe`. Remove it to undo.
- Optional: `ForceBridge = true` under `[LobbyServer]` in `data\lobby\config\LobbySettings.ini` always
  hosts through the bridge, without the reachability test.
- Optional: `bin\sadk_bridge.ini` `[Bridge] port=` overrides the stub's bridge port (default 7072).
