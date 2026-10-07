# wsock32 bridge shim

A proxy `wsock32.dll` that lets a host behind NAT be joined through the lobby server. Protocol and the
evidence behind it: `docs/bridge-protocol.md`; the server side is `sadk_lobby/bridge.py`.

`tincat3.dll` imports all of its sockets from `WSOCK32.dll` by ordinal; `SADK.exe` uses `WS2_32.dll`
directly, so the shim only sees TinCat traffic. `WSOCK32.dll` is not a KnownDLL, so Windows loads a copy
from the game folder before the system one (live 2026-10-07: loaded, all exports resolved).

Every export is forwarded by ordinal to the system `wsock32.dll`; `connect`, `send`, `listen` and
`closesocket` are hooked. The shim logs to `wsock32_shim.txt` next to the game exe.

- Build (Linux, mingw-w64): `make` -> `wsock32.dll`.
- Install: copy it into the game's `bin` folder, next to `SADK.exe`. Remove it to undo.
- Optional: `ForceBridge = true` under `[LobbyServer]` in `data\lobby\config\LobbySettings.ini` always
  hosts through the bridge, without the reachability test.
- Optional: `bin\sadk_bridge.ini` `[Bridge] port=` overrides the stub's bridge port (default 7072).
